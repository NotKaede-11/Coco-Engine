#include "search.h"
#include "types.h"
#include "movegen.h"
#include "tt.h"
#include "evaluate.h"
#include "tbprobe.h"
#include <iostream>

// Constants for evaluation and search bounds
// Decisive tablebase scores must remain below the mate band so that UCI mate
// conversion, TT score normalization, and mate-distance pruning cannot confuse
// a DTZ result with a forced checkmate.
const int VALUE_TB = 28000;
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

// Constants for evaluation and search bounds
const int MATE_SCORE = 30000;
const int MATE_THRESHOLD = 29000;

// Nodes visited counter
thread_local uint64_t nodes_visited = 0;
thread_local int active_thread_id = 0;

// Maximum ply reached in search (for seldepth reporting)
thread_local int max_ply_reached = 0;
thread_local Move root_excluded_moves[256];
thread_local int root_excluded_count = 0;
thread_local Move root_search_best_move;
thread_local TranspositionTable* thread_tt = &tt;
thread_local bool root_accounting_enabled = false;
thread_local int currmove_report_depth = -1;
thread_local Move reported_currmoves[256];
thread_local int reported_currmove_count = 0;

inline TranspositionTable& search_tt() {
    return *thread_tt;
}

struct RootNodeStat {
    Move move;
};

RootNodeStat root_node_stats[256];
uint64_t root_node_counts[MAX_THREADS][256]{};
int root_node_stat_count = 0;
Move root_allowed_moves[256];
int root_allowed_count = 0;
bool root_moves_restricted = false;

inline void record_root_nodes(Move move, uint64_t nodes) {
    if (!root_accounting_enabled) return;
    for (int i = 0; i < root_node_stat_count; ++i) {
        if (root_node_stats[i].move == move) {
            root_node_counts[active_thread_id][i] += nodes;
            return;
        }
    }
}

inline uint64_t root_nodes_for_move(Move move) {
    for (int i = 0; i < root_node_stat_count; ++i) {
        if (root_node_stats[i].move == move)
            return root_node_counts[0][i];
    }
    return 0;
}
#ifdef COCO_TESTING
thread_local int nmp_test_attempts = 0;
thread_local int nmp_test_cutoffs = 0;
#endif

// Time utility in milliseconds
inline uint64_t get_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

// Check time and trigger abort if hard boundary is exceeded
inline void check_time() {
    if ((nodes_visited & Search::time_check_mask) != 0)
        return;

    // Publish progress only at an existing periodic stop check.  This keeps
    // multi-thread node limits accurate without an atomic increment per node.
    Search::thread_stats[active_thread_id].nodes.store(nodes_visited, std::memory_order_relaxed);
    Search::thread_stats[active_thread_id].seldepth.store(max_ply_reached, std::memory_order_relaxed);

    if (Search::node_limit != 0) {
        uint64_t total_nodes = 0;
        for (int t = 0; t < Search::num_threads; ++t)
            total_nodes += Search::thread_stats[t].nodes.load(std::memory_order_relaxed);

        if (total_nodes >= Search::node_limit) {
            Search::b_abort.store(true, std::memory_order_relaxed);
            return;
        }
    }

    if (!Search::pondering.load(std::memory_order_relaxed) && Search::hard_limit != 0) {
        uint64_t elapsed = get_time_ms() - Search::start_time.load(std::memory_order_relaxed);
        if (elapsed >= Search::hard_limit) {
            Search::b_abort.store(true, std::memory_order_relaxed);
        }
    }
}

// Move string converter helper
std::string move_to_str(Move m) {
    if (m.is_none()) return "0000";
    std::string s = square_to_str(m.from()) + square_to_str(m.to());
    if (m.is_promotion()) {
        int pt = m.promotion_piece_type();
        if (pt == KNIGHT) s += "n";
        else if (pt == BISHOP) s += "b";
        else if (pt == ROOK) s += "r";
        else if (pt == QUEEN) s += "q";
    }
    return s;
}

const int MAX_PLY = 128;
thread_local Move killer_moves[MAX_PLY][2];
thread_local Color root_color = WHITE;
thread_local int16_t history_table[2][2][2][7][64]; // [color][threat_from][threat_to][piece_type][to_square] - size 7 to prevent out-of-bounds on NO_PIECE_TYPE
int lmr_table[64][64];

using Search::NodeType;

struct SearchStack {
    int piece = -1;
    int to_sq = -1;
    int static_eval = INFINITY_SCORE;
    Move current_move;
};
thread_local SearchStack search_stack[MAX_PLY + 4];
thread_local int16_t cont_history[2][7][64][7][64];
thread_local int16_t capture_history[12][64][6];

inline bool root_move_is_excluded(Move move) {
    for (int i = 0; i < root_excluded_count; ++i) {
        if (root_excluded_moves[i] == move) return true;
    }
    return false;
}

inline bool root_move_is_allowed(Move move) {
    if (!root_moves_restricted) return true;
    for (int i = 0; i < root_allowed_count; ++i)
        if (root_allowed_moves[i] == move) return true;
    return false;
}

inline bool mate_within_limit(int score) {
    if (Search::mate_limit <= 0 || std::abs(score) <= MATE_THRESHOLD) return false;
    const int plies = MATE_SCORE - std::abs(score);
    return plies <= Search::mate_limit * 2;
}

struct WdlPermille { int win; int draw; int loss; };

inline WdlPermille score_to_wdl(int score) {
    if (score > MATE_THRESHOLD) return {1000, 0, 0};
    if (score < -MATE_THRESHOLD) return {0, 0, 1000};
    const double bounded = std::clamp(static_cast<double>(score), -2000.0, 2000.0);
    const auto logistic = [](double value) { return 1.0 / (1.0 + std::exp(-value)); };
    int win = static_cast<int>(std::lround(1000.0 * logistic((bounded - 100.0) / 180.0)));
    int loss = static_cast<int>(std::lround(1000.0 * logistic((-bounded - 100.0) / 180.0)));
    if (win + loss > 1000) {
        const double scale = 1000.0 / static_cast<double>(win + loss);
        win = static_cast<int>(std::lround(win * scale));
        loss = 1000 - win;
    }
    return {win, 1000 - win - loss, loss};
}

inline void print_uci_score(int score) {
    if (score > MATE_THRESHOLD) {
        const int plies = MATE_SCORE - score;
        std::cout << "score mate " << ((plies + 1) / 2);
    } else if (score < -MATE_THRESHOLD) {
        const int plies = score + MATE_SCORE;
        std::cout << "score mate -" << ((plies + 1) / 2);
    } else {
        std::cout << "score cp " << score;
    }
    if (Search::UCI_ShowWDL) {
        const WdlPermille wdl = score_to_wdl(score);
        std::cout << " wdl " << wdl.win << " " << wdl.draw << " " << wdl.loss;
    }
}

inline uint64_t aggregate_tbhits() {
    uint64_t total = 0;
    for (int thread = 0; thread < Search::num_threads; ++thread)
        total += Search::thread_stats[thread].tbhits.load(std::memory_order_relaxed);
    return total;
}

inline void update_history(int16_t& entry, int bonus) {
    int val = entry;
    val += bonus - val * std::abs(bonus) / 32768;
    entry = static_cast<int16_t>(std::clamp(val, -30000, 30000));
}

inline int draw_score(const Board& board) {
    return board.get_side_to_move() == root_color ? -Search::Contempt : Search::Contempt;
}


// Map Fathom move representation to Coco's Move class
Move fathom_to_coco_move(const Board& board, unsigned fathom_res) {
    int from = TB_GET_FROM(fathom_res);
    int to = TB_GET_TO(fathom_res);
    int promotes = TB_GET_PROMOTES(fathom_res);
    bool ep = TB_GET_EP(fathom_res);
    
    bool is_cap = (board.get_piece_at(to) != NO_PIECE) || ep;
    
    int flags = FLAG_QUIET;
    if (ep) {
        flags = FLAG_EP;
    } else if (promotes != TB_PROMOTES_NONE) {
        int pt = KNIGHT;
        if (promotes == TB_PROMOTES_QUEEN) pt = QUEEN;
        else if (promotes == TB_PROMOTES_ROOK) pt = ROOK;
        else if (promotes == TB_PROMOTES_BISHOP) pt = BISHOP;
        
        flags = (pt - 1) + 8;
        if (is_cap) {
            flags += 4;
        }
    } else if (is_cap) {
        flags = FLAG_CAPTURE;
    } else {
        Piece p = board.get_piece_at(from);
        if ((p == W_PAWN || p == B_PAWN) && std::abs(from - to) == 16) {
            flags = FLAG_DOUBLE_PAWN;
        }
    }
    
    return Move(from, to, flags);
}

// Convert Coco board to Fathom bitboards and call tb_probe_wdl
unsigned probe_wdl(const Board& board) {
    U64 white = board.get_occupancy(WHITE);
    U64 black = board.get_occupancy(BLACK);
    U64 kings = board.get_pieces(WHITE, KING) | board.get_pieces(BLACK, KING);
    U64 queens = board.get_pieces(WHITE, QUEEN) | board.get_pieces(BLACK, QUEEN);
    U64 rooks = board.get_pieces(WHITE, ROOK) | board.get_pieces(BLACK, ROOK);
    U64 bishops = board.get_pieces(WHITE, BISHOP) | board.get_pieces(BLACK, BISHOP);
    U64 knights = board.get_pieces(WHITE, KNIGHT) | board.get_pieces(BLACK, KNIGHT);
    U64 pawns = board.get_pieces(WHITE, PAWN) | board.get_pieces(BLACK, PAWN);
    unsigned ep = board.get_en_passant_square();
    if (ep == SQ_NONE) ep = 0;
    bool turn = (board.get_side_to_move() == WHITE);
    
    unsigned castling = board.get_castling_rights();
    unsigned rule50 = Search::Syzygy50MoveRule ? board.get_halfmove_clock() : 0;
    
    return tb_probe_wdl(white, black, kings, queens, rooks, bishops, knights, pawns, rule50, castling, ep, turn);
}


// Helper to retrieve the combined quiet history score for a move
inline int get_quiet_history_score(const Board& board, Move move, int ply, U64 threats) {
    Color side = board.get_side_to_move();
    Piece piece_raw = board.get_piece_at(move.from());
    PieceType piece = (PieceType)(piece_raw % 6);
    int to = move.to();
    bool tf = threats & (1ULL << move.from());
    bool tt = threats & (1ULL << move.to());
    
    int score = 0;
    if (piece < 7) {
        score += history_table[side][tf][tt][piece][to];
        
        if (ply >= 1 && ply - 1 < MAX_PLY) {
            const SearchStack& ni1 = search_stack[ply - 1];
            if (ni1.piece >= 0 && ni1.piece < 7) {
                score += cont_history[0][ni1.piece][ni1.to_sq][piece][to];
            }
        }
        
        if (ply >= 2 && ply - 2 < MAX_PLY) {
            const SearchStack& ni2 = search_stack[ply - 2];
            if (ni2.piece >= 0 && ni2.piece < 7) {
                score += cont_history[1][ni2.piece][ni2.to_sq][piece][to];
            }
        }
    }
    return score;
}

// Move Ordering helper function using stack memory
void order_moves(const Board& board, const MoveList& move_list, Move tt_move, int ply, int* scores, U64 threats = 0) {
    const int mvv_lva_values[6] = { 100, 320, 330, 500, 900, 20000 };
    
    Color side = board.get_side_to_move();
    
    for (int i = 0; i < move_list.count; i++) {
        Move move = move_list.moves[i];
        
        if (move == tt_move) {
            scores[i] = 10000000;
        } else if (move.is_capture()) {
            int victim = PAWN;
            if (!move.is_en_passant()) {
                victim = board.get_piece_at(move.to()) % 6;
            }
            int assailant = board.get_piece_at(move.from()) % 6;
            int promotion_gain = 0;
            if (move.is_promotion()) {
                promotion_gain = mvv_lva_values[move.promotion_piece_type()]
                               - mvv_lva_values[PAWN];
            }
            int capture_value = (mvv_lva_values[victim] + promotion_gain) * 10 - assailant;
            
            int moved_piece = board.get_piece_at(move.from());
            int cap_hist = 0;
            if (moved_piece < 12 && victim < 6) {
                cap_hist = capture_history[moved_piece][move.to()][victim];
            }
            
            if (mvv_lva_values[victim] >= mvv_lva_values[assailant]) {
                scores[i] = 1000000 + capture_value + cap_hist;
            } else {
                scores[i] = 10000 + capture_value + cap_hist;
            }
        } else if (move.is_promotion()) {
            int promo = move.promotion_piece_type();
            scores[i] = 950000 + mvv_lva_values[promo];
        } else {
            // Quiet move
            if (ply < MAX_PLY && move == killer_moves[ply][0]) {
                scores[i] = 900000;
            } else if (ply < MAX_PLY && move == killer_moves[ply][1]) {
                scores[i] = 800000;
            } else {
                scores[i] = get_quiet_history_score(board, move, ply, threats);
            }
        }
    }
}

// Stateful ordering boundary. The baseline picker deliberately reproduces
// the former incremental selection-sort order exactly; later stage changes
// can therefore be measured without also changing the caller's control flow.
class MovePicker {
public:
    MovePicker(const Board& board, MoveList& moves, Move tt_move, int ply, U64 threats = 0)
        : moves_(moves) {
        order_moves(board, moves_, tt_move, ply, scores_, threats);
    }

    bool next(Move& move) {
        if (index_ >= moves_.count)
            return false;

        int best = index_;
        for (int i = index_ + 1; i < moves_.count; ++i) {
            if (scores_[i] > scores_[best])
                best = i;
        }
        if (best != index_) {
            std::swap(moves_.moves[index_], moves_.moves[best]);
            std::swap(scores_[index_], scores_[best]);
        }
        move = moves_.moves[index_++];
        return true;
    }

private:
    MoveList& moves_;
    int scores_[256]{};
    int index_ = 0;
};

// Forward declarations of search functions
int quiescence(Board& board, int alpha, int beta, int ply);

// Extract Principal Variation (PV) from the Transposition Table
int get_pv(Board& board, Move* pv_array, int max_pv_depth) {
    int pv_length = 0;
    U64 key = board.get_hash_key();
    int score;
    Move best_move;
    
    while (pv_length < max_pv_depth) {
        // Probe TT with depth 0 to retrieve best move without score restriction
        if (!search_tt().probe(key, score, best_move, 0, -INFINITY_SCORE, INFINITY_SCORE, 0) || best_move.is_none()) {
            break;
        }
        
        // Validate best move legally against the board state
        MoveList list;
        generate_pseudo_legal_moves(board, list);
        bool found = false;
        for (int i = 0; i < list.count; i++) {
            if (list.moves[i] == best_move) {
                found = true;
                break;
            }
        }
        if (!found) break;
        
        pv_array[pv_length++] = best_move;
        
        if (!board.make_move(best_move)) {
            pv_length--;
            break;
        }

        // A PV ends when the game ends.  TT entries can otherwise form a
        // legal repetition cycle and make GUIs/fastchess report a bogus
        // continuation after the draw has already been reached.
        if (board.get_halfmove_clock() >= 100 || board.is_repetition()) {
            break;
        }
        
        key = board.get_hash_key();
    }
    
    // Restore the board back to the original root state
    for (int i = pv_length - 1; i >= 0; i--) {
        board.unmake_move(pv_array[i]);
    }
    
    return pv_length;
}

// Alpha-Beta Search Core with Null Move Pruning (NMP)
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, NodeType node_type, bool in_null_move_search, int parent_eval_1, int parent_eval_2, Move excluded_move, int double_ext) {
    const bool is_pv = node_type == NodeType::PV;
    // Cooperative search abortion check
    check_time();
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
    
    // Draw detection (Fifty-move rule and repetition check)
    if (ply > 0 && (board.get_halfmove_clock() >= 100 || board.is_repetition())) {
        return draw_score(board);
    }

    if (depth <= 0) {
        return quiescence(board, alpha, beta, ply);
    }

    nodes_visited++;
    if (ply > max_ply_reached) {
        max_ply_reached = ply;
    }
    
    // Syzygy WDL Probing at non-root nodes
    int num_pieces = count_bits(board.get_occupancy(BOTH));
    if (TB_LARGEST > 0 
        && num_pieces <= (int)TB_LARGEST 
        && ply > 0 
        && board.get_castling_rights() == 0 
        && (!Search::Syzygy50MoveRule || board.get_halfmove_clock() == 0)
        && excluded_move.is_none()
        && (num_pieces < (int)TB_LARGEST || !Search::SyzygyProbeLimit || depth >= Search::SyzygyProbeDepth)) 
    {
        unsigned wdl = probe_wdl(board);
        if (wdl != TB_RESULT_FAILED) {
            Search::thread_stats[active_thread_id].tbhits.fetch_add(1, std::memory_order_relaxed);
            int tb_score = 0;
            if (wdl == TB_WIN) {
                tb_score = VALUE_TB - ply;
            } else if (wdl == TB_CURSED_WIN) {
                tb_score = 1;
            } else if (wdl == TB_DRAW) {
                tb_score = 0;
            } else if (wdl == TB_BLESSED_LOSS) {
                tb_score = -1;
            } else if (wdl == TB_LOSS) {
                tb_score = -VALUE_TB + ply;
            }
            
            uint8_t flag = HASH_EXACT;
            if (tb_score >= beta) {
                flag = HASH_BETA;
            } else if (tb_score <= alpha) {
                flag = HASH_ALPHA;
            }
            
            if (excluded_move.is_none()) {
                search_tt().store(board.get_hash_key(), Move(), tb_score, depth, flag, ply);
            }
            return tb_score;
        }
    }
    
    Color us = board.get_side_to_move();
    Move quiet_moves_searched[64];
    int quiet_count = 0;
    Move capture_moves_searched[64];
    int capture_count = 0;
    int king_sq = get_lsb(board.get_pieces(us, KING));
    bool in_check = board.is_square_attacked(king_sq, us ^ 1);

    // NMP and the later forward-pruning stages share one static evaluation.
    int static_eval = Evaluation::evaluate(board);
    if (ply < MAX_PLY + 4) search_stack[ply].static_eval = static_eval;

    // Compute improving before any evaluation-based pruning. Checks inherit
    // the last usable evaluations rather than treating a missing value as an
    // improvement signal.
    bool improving = false;
    if (!in_check && parent_eval_2 != INFINITY_SCORE) {
        improving = static_eval > parent_eval_2;
    }

    int next_parent_eval_1 = in_check ? parent_eval_1 : static_eval;
    int next_parent_eval_2 = in_check ? parent_eval_2 : parent_eval_1;

    // Null Move Pruning (NMP)
    if (Search::mate_limit == 0 && depth >= 3 && !is_pv && !in_check && !in_null_move_search
        && excluded_move.is_none() && static_eval >= beta
        && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        const int minor_count = count_bits(board.get_pieces(us, KNIGHT)
                                          | board.get_pieces(us, BISHOP));
        const bool has_heavy = board.get_pieces(us, ROOK) || board.get_pieces(us, QUEEN);
        // Pawn-only and lone-minor endings are the classic zugzwang danger
        // zone. Requiring a heavy piece or two minors is deliberately more
        // conservative than the old any-non-pawn test.
        if (has_heavy || minor_count >= 2) {
            const int eval_bonus = std::clamp((static_eval - beta) / 200, 0, 3);
            int R = Search::NMP_Base + (depth / Search::NMP_Divisor) + eval_bonus;
            R = std::clamp(R, 2, depth - 1);
            if (ply < MAX_PLY) {
                search_stack[ply].piece = -1;
                search_stack[ply].to_sq = -1;
                search_stack[ply].current_move = Move();
            }
            if (board.make_null_move()) {
#ifdef COCO_TESTING
                if (ply == 0) ++nmp_test_attempts;
#endif
                int null_score = -alpha_beta(board, -beta, -beta + 1, depth - 1 - R, ply + 1, NodeType::NON_PV, true, parent_eval_1, parent_eval_2, excluded_move, double_ext);
                board.unmake_null_move();

                if (null_score >= beta) {
#ifdef COCO_TESTING
                    if (ply == 0) ++nmp_test_cutoffs;
#endif
                    return beta;
                }
            }
        }
    }
    
    // Probe the Transposition Table
    int tt_score = 0;
    uint8_t tt_depth = 0;
    uint8_t tt_flag = 0;
    Move tt_move;
    bool tt_hit = search_tt().probe_entry(board.get_hash_key(), tt_score, tt_depth, tt_flag, tt_move, ply);
    
    const bool restricted_root = ply == 0
        && (root_excluded_count > 0 || root_moves_restricted);
    if (tt_hit && tt_move != excluded_move && !restricted_root) {
        if (tt_depth >= depth) {
            if (tt_flag == HASH_EXACT) {
                if (ply == 0) root_search_best_move = tt_move;
                return tt_score;
            }
            if (tt_flag == HASH_ALPHA && tt_score <= alpha) {
                if (ply == 0) root_search_best_move = tt_move;
                return tt_score;
            }
            if (tt_flag == HASH_BETA && tt_score >= beta) {
                if (ply == 0) root_search_best_move = tt_move;
                return tt_score;
            }
        }
    }
    if (tt_move == excluded_move
        || (ply == 0 && (!root_move_is_allowed(tt_move) || root_move_is_excluded(tt_move)))) {
        tt_move = Move();
    }
    
    // Internal Iterative Reductions (IIR)
    if (Search::mate_limit == 0 && is_pv && depth >= 3 && tt_move.is_none()) {
        depth--;
    }

    // Reverse Futility Pruning (RFP) keeps its accepted post-IIR depth
    // semantics while the strict-order alternative is evaluated separately.
    if (Search::mate_limit == 0 && depth <= 3 && !is_pv && !in_check && excluded_move.is_none()
        && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        int margin = Search::RFP_Margin * depth - (improving ? 35 : 0);
        if (static_eval - margin >= beta) {
            return static_eval;
        }
    }

    // Razoring follows RFP with the post-IIR depth, preserving the calibrated
    // T4 selectivity boundary established by the accepted baseline.
    if (Search::mate_limit == 0 && depth == 1 && !is_pv && !in_check && excluded_move.is_none()
        && alpha > -INFINITY_SCORE + 1000) {
        constexpr int razor_margin = 300;
        if (static_eval + razor_margin <= alpha) {
            int q_score = quiescence(board, alpha, beta, ply);
            if (q_score <= alpha) {
                return q_score;
            }
        }
    }

    // Internal Iterative Deepening (IID)
    if (tt_move.is_none() && depth >= 4 && !in_check && is_pv) {
        int iid_depth = depth - 2;
        alpha_beta(board, alpha, beta, iid_depth, ply, node_type, in_null_move_search, parent_eval_1, parent_eval_2, excluded_move, double_ext);
        // Time-abort check: immediately return 0 and skip probing the TT to prevent move pollution
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
        int dummy_score;
        search_tt().probe(board.get_hash_key(), dummy_score, tt_move, 0, -INFINITY_SCORE, INFINITY_SCORE, ply);
    }
    
    int alpha_orig = alpha;
    
    LegalityMasks masks = board.get_legality_masks();
    
    MoveList move_list;
    if (in_check)
        generate_evasion_moves(board, move_list);
    else
        generate_pseudo_legal_moves(board, move_list);
    MovePicker move_picker(board, move_list, tt_move, ply, masks.threats);
    
    int legal_moves_count = 0;
    int best_score = -INFINITY_SCORE;
    Move best_move_in_node;
    int moves_searched = 0;
 
    Move move;
    while (move_picker.next(move)) {
        
        if (move == excluded_move) {
            continue;
        }
        if (ply == 0 && (!root_move_is_allowed(move) || root_move_is_excluded(move))) {
            continue;
        }
        
        // Late Move Pruning (LMP)
        if (Search::mate_limit == 0 && !move.is_capture() && !move.is_promotion()) {
            if (depth <= 3 && !is_pv && !in_check) {
                bool from_threatened = masks.threats & (1ULL << move.from());
                if (!from_threatened) {  // Only prune non-escaping moves
                    int move_threshold = 4 + (depth * depth);
                    if (moves_searched >= move_threshold) {
                        break;
                    }
                }
            }
        }
        
        // Move-Level Futility Pruning
        // Strictly disabled if alpha is near a checkmate score to prevent mate-blindness tactical leaks
        if (Search::mate_limit == 0 && depth <= 2 && !in_check
            && !move.is_capture() && !move.is_promotion()) {
            bool from_threatened = masks.threats & (1ULL << move.from());
            if (!from_threatened && move != tt_move && 
                (ply >= MAX_PLY || (move != killer_moves[ply][0] && move != killer_moves[ply][1])) &&
                std::abs(alpha) < MATE_SCORE - MAX_PLY) {
                int margin = 100 * depth;
                if (static_eval + margin <= alpha) {
                    continue;
                }
            }
        }
        
        // Static Exchange Evaluation (SEE) Quiet Pruning
        if (Search::mate_limit == 0 && !move.is_capture() && !move.is_promotion()
            && depth <= Search::SEE_Pruning_Depth && !in_check) {
            if (board.see(move) < 0) {
                continue;
            }
        }
        
        // Symmetrical illegal move filtering
        if (!board.is_move_legal(move, masks)) {
            continue;
        }

        if (ply == 0 && active_thread_id == 0) {
            const uint64_t elapsed = get_time_ms()
                - Search::start_time.load(std::memory_order_relaxed);
            if (elapsed >= 3000) {
                if (currmove_report_depth != depth) {
                    currmove_report_depth = depth;
                    reported_currmove_count = 0;
                }
                bool already_reported = false;
                for (int i = 0; i < reported_currmove_count; ++i)
                    if (reported_currmoves[i] == move) { already_reported = true; break; }
                if (!already_reported && reported_currmove_count < 256) {
                    reported_currmoves[reported_currmove_count++] = move;
                    std::cout << "info depth " << depth << " currmove " << move_to_str(move)
                              << " currmovenumber " << (legal_moves_count + 1) << "\n";
                }
            }
        }

        int extension = 0;

        // Singular Extension Check
        if (ply > 0
            && depth >= 6 + is_pv
            && move == tt_move
            && excluded_move.is_none()
            && tt_hit
            && tt_depth >= depth - 3
            && tt_flag != HASH_ALPHA
            && std::abs(tt_score) < MATE_THRESHOLD) {
            
            int singular_margin = depth * (is_pv ? 1 : 2);
            int singular_beta = tt_score - singular_margin;
            int singular_depth = depth / 2;
            
            int singular_score = alpha_beta(board, singular_beta - 1, singular_beta, singular_depth, ply, NodeType::NON_PV, in_null_move_search, parent_eval_1, parent_eval_2, tt_move, double_ext);
            
            if (singular_score < singular_beta) {
                // Move is singular: extend by 1. Double-extend (+2) when it is
                // strongly singular (verified well below the singular bound) and
                // only on non-PV nodes, capped to prevent runaway depth inflation.
                if (!is_pv && double_ext < 6 && singular_score < singular_beta - 2 * depth) {
                    extension = 2;
                } else {
                    extension = 1;
                }
            } else if (singular_score >= beta && std::abs(singular_score) < MATE_THRESHOLD) {
                // Multicut: excluding the TT move still fails high over beta, so
                // multiple moves compete. Return immediately as a softbound.
                return singular_score;
            }
        }

        // Propagate cumulative double-extension count along the main search path
        int next_double_ext = double_ext + (extension >= 2 ? 1 : 0);
        
        if (ply < MAX_PLY) {
            if (!move.is_capture() && !move.is_promotion()) {
                Piece p_raw = board.get_piece_at(move.from());
                PieceType p_type = (p_raw != NO_PIECE) ? (PieceType)(p_raw % 6) : NO_PIECE_TYPE;
                search_stack[ply].piece = (int)p_type;
                search_stack[ply].to_sq = move.to();
                search_stack[ply].current_move = move;
            } else {
                search_stack[ply].piece = -1;
                search_stack[ply].to_sq = -1;
                search_stack[ply].current_move = move;
            }
        }
        
        const uint64_t root_nodes_before = ply == 0 ? nodes_visited : 0;
        board.make_move(move, true);
        search_tt().prefetch(board.get_hash_key());
        legal_moves_count++;
        
        // Check if this move gives check
        Color them = board.get_side_to_move();
        int them_king_sq = get_lsb(board.get_pieces(them, KING));
        bool gives_check = board.is_square_attacked(them_king_sq, them ^ 1);

        const int new_depth = depth - 1 + extension;
        int score;
        if (moves_searched == 0) {
            score = -alpha_beta(board, -beta, -alpha, new_depth, ply + 1,
                                node_type, in_null_move_search,
                                next_parent_eval_1, next_parent_eval_2,
                                Move(), next_double_ext);
        } else {
            int reduction = 0;

            // Late quiet moves at non-PV nodes are first searched at reduced
            // depth with a null window. Any reduced fail-high must be verified
            // at full depth before it can change alpha or produce a cutoff.
            // The existing reduction constants were tuned for non-PV nodes;
            // PV-node LMR is therefore kept disabled until separately tuned.
            if (depth >= 3 && !move.is_capture() && !move.is_promotion() && !in_check &&
                move != tt_move && !gives_check && moves_searched >= 4 && !is_pv) {
                
                int d_idx = std::min(depth, 63);
                int m_idx = std::min(moves_searched, 63);
                reduction = lmr_table[d_idx][m_idx];
                bool has_history = parent_eval_2 != INFINITY_SCORE;
                if (has_history && !improving) {
                    reduction++;
                }
                
                // Threats-based LMR adjustments
                bool from_threatened = masks.threats & (1ULL << move.from());
                bool to_threatened   = masks.threats & (1ULL << move.to());
                if (from_threatened) {
                    reduction--;
                }
                if (to_threatened) {
                    reduction++;
                }
                
                // History-based LMR adjustments (quiet history + CMH + FMH)
                int hist_score = get_quiet_history_score(board, move, ply, masks.threats);
                int hist_adj = hist_score / Search::LMR_History_Divisor;
                hist_adj = std::max(0, std::min(1, hist_adj));
                reduction -= hist_adj;
                
                reduction = std::clamp(reduction, 0, std::max(0, new_depth - 1));
            }

            if (reduction > 0) {
                score = -alpha_beta(board, -alpha - 1, -alpha,
                                    new_depth - reduction, ply + 1,
                                    NodeType::NON_PV, in_null_move_search,
                                    next_parent_eval_1, next_parent_eval_2,
                                    Move(), next_double_ext);

            } else {
                score = -alpha_beta(board, -alpha - 1, -alpha,
                                    new_depth, ply + 1, NodeType::NON_PV,
                                    in_null_move_search, next_parent_eval_1,
                                    next_parent_eval_2, Move(), next_double_ext);
            }
            
            // Principal variation search: only a late move that improves alpha
            // inside the PV window needs the expensive full-window re-search.
            if (score > alpha && score < beta) {
                score = -alpha_beta(board, -beta, -alpha, new_depth, ply + 1,
                                    node_type, in_null_move_search,
                                    next_parent_eval_1, next_parent_eval_2,
                                    Move(), next_double_ext);
            }
        }
        
        board.unmake_move(move);

        if (ply == 0) {
            record_root_nodes(move, nodes_visited - root_nodes_before);
        }
        
        // Safely abort this recursive branch
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
        
        // Record quiet moves that did not trigger a cutoff
        if (score < beta) {
            if (move.is_capture() && capture_count < 64) {
                capture_moves_searched[capture_count++] = move;
            } else if (!move.is_capture() && !move.is_promotion() && quiet_count < 64) {
                quiet_moves_searched[quiet_count++] = move;
            }
        }
        
        moves_searched++;
        
        if (score > best_score) {
            best_score = score;
            best_move_in_node = move;
        }
        
        if (score > alpha) {
            alpha = score;
            if (alpha >= beta) {
                // Store beta cutoff in TT
                if (excluded_move.is_none()) {
                    if (!restricted_root)
                        search_tt().store(board.get_hash_key(), move, score, depth, HASH_BETA, ply);
                }
                
                // Update killer moves and history heuristic for quiet moves
                if (excluded_move.is_none() && ply < MAX_PLY) {
                    if (!move.is_capture() && !move.is_promotion()) {
                        // Update Killer Moves
                        if (killer_moves[ply][0] != move) {
                            killer_moves[ply][1] = killer_moves[ply][0];
                            killer_moves[ply][0] = move;
                        }
                        
                        // Update History Heuristic Table
                        Color side = board.get_side_to_move();
                        Piece piece_raw = board.get_piece_at(move.from());
                        PieceType piece = (PieceType)(piece_raw % 6);
                        int to = move.to();
                        
                        if (piece < 7) {
                            bool tf = masks.threats & (1ULL << move.from());
                            bool tt = masks.threats & (1ULL << move.to());
                            update_history(history_table[side][tf][tt][piece][to], depth * depth);
                            
                            // CMH (ply-1)
                            if (ply >= 1 && ply - 1 < MAX_PLY) {
                                const SearchStack& ni1 = search_stack[ply - 1];
                                if (ni1.piece >= 0 && ni1.piece < 7) {
                                    update_history(cont_history[0][ni1.piece][ni1.to_sq][piece][to], depth * depth);
                                }
                            }

                            // FMH (ply-2)
                            if (ply >= 2 && ply - 2 < MAX_PLY) {
                                const SearchStack& ni2 = search_stack[ply - 2];
                                if (ni2.piece >= 0 && ni2.piece < 7) {
                                    update_history(cont_history[1][ni2.piece][ni2.to_sq][piece][to], depth * depth);
                                }
                            }
                            
                            // Apply malus loop for unsuccessful quiet moves
                            for (int i = 0; i < quiet_count; i++) {
                                Move failed_move = quiet_moves_searched[i];
                                Piece failed_piece_raw = board.get_piece_at(failed_move.from());
                                PieceType failed_piece = (PieceType)(failed_piece_raw % 6);
                                int failed_to_sq = failed_move.to();
                                if (failed_piece < 7) {
                                    bool ftf = masks.threats & (1ULL << failed_move.from());
                                    bool ftt = masks.threats & (1ULL << failed_move.to());
                                    update_history(history_table[side][ftf][ftt][failed_piece][failed_to_sq], -(depth * depth));
                                    
                                    // CMH malus
                                    if (ply >= 1 && ply - 1 < MAX_PLY) {
                                        const SearchStack& ni1 = search_stack[ply - 1];
                                        if (ni1.piece >= 0 && ni1.piece < 7) {
                                            update_history(cont_history[0][ni1.piece][ni1.to_sq][failed_piece][failed_to_sq], -(depth * depth));
                                        }
                                    }

                                    // FMH malus
                                    if (ply >= 2 && ply - 2 < MAX_PLY) {
                                        const SearchStack& ni2 = search_stack[ply - 2];
                                        if (ni2.piece >= 0 && ni2.piece < 7) {
                                            update_history(cont_history[1][ni2.piece][ni2.to_sq][failed_piece][failed_to_sq], -(depth * depth));
                                        }
                                    }
                                }
                            }
                        }
                    } else if (move.is_capture()) {
                        // Capture History Update
                        Piece piece_raw = board.get_piece_at(move.from());
                        int moved_piece = piece_raw;
                        int to = move.to();
                        int victim = PAWN;
                        if (!move.is_en_passant()) {
                            victim = board.get_piece_at(move.to()) % 6;
                        }
                        
                        if (moved_piece < 12 && victim < 6) {
                            update_history(capture_history[moved_piece][to][victim], depth * depth);
                            
                            // Apply malus loop for unsuccessful capture moves
                            for (int i = 0; i < capture_count; i++) {
                                Move failed_move = capture_moves_searched[i];
                                Piece failed_piece_raw = board.get_piece_at(failed_move.from());
                                int failed_moved_piece = failed_piece_raw;
                                int failed_to_sq = failed_move.to();
                                int failed_victim = PAWN;
                                if (!failed_move.is_en_passant()) {
                                    failed_victim = board.get_piece_at(failed_to_sq) % 6;
                                }
                                if (failed_moved_piece < 12 && failed_victim < 6) {
                                    update_history(capture_history[failed_moved_piece][failed_to_sq][failed_victim], -(depth * depth));
                                }
                            }
                        }
                    }
                }
                
                if (ply == 0) root_search_best_move = move;
                return score;
            }
        }
    }
    
    // Terminal position detection
    if (legal_moves_count == 0) {
        if (in_check) {
            // Checkmate: score depends on distance from root (prefers shorter mates)
            return -MATE_SCORE + ply;
        } else {
            // Stalemate
            return draw_score(board);
        }
    }
    
    // Save search result in Transposition Table
    uint8_t flag = HASH_EXACT;
    if (best_score <= alpha_orig) {
        flag = HASH_ALPHA;
    }
    if (ply == 0) root_search_best_move = best_move_in_node;
    if (excluded_move.is_none() && !restricted_root) {
        search_tt().store(board.get_hash_key(), best_move_in_node, best_score, depth, flag, ply);
    }
    
    return best_score;
}

// Quiescence Search
int quiescence(Board& board, int alpha, int beta, int ply) {
    check_time();
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;

    if (ply > 0 && (board.get_halfmove_clock() >= 100 || board.is_repetition()))
        return draw_score(board);

    
    nodes_visited++;
    if (ply > max_ply_reached) {
        max_ply_reached = ply;
    }
    
    if (ply >= MAX_PLY - 1)
        return Evaluation::evaluate(board);

    const Color us = board.get_side_to_move();
    const U64 king = board.get_pieces(us, KING);
    if (!king) return -MATE_SCORE + ply;
    const bool in_check = board.is_square_attacked(get_lsb(king), us ^ 1);
    const int alpha_orig = alpha;

    int tt_score = 0;
    uint8_t tt_depth = 0, tt_flag = HASH_EXACT;
    Move tt_move;
    if (search_tt().probe_entry(board.get_hash_key(), tt_score, tt_depth, tt_flag, tt_move, ply)) {
        if (tt_flag == HASH_EXACT) return tt_score;
        if (tt_flag == HASH_ALPHA && tt_score <= alpha) return tt_score;
        if (tt_flag == HASH_BETA && tt_score >= beta) return tt_score;
    }

    int stand_pat = -INFINITY_SCORE;
    int best_score = -INFINITY_SCORE;
    if (!in_check) {
        stand_pat = Evaluation::evaluate(board);
        best_score = stand_pat;
        if (stand_pat >= beta) {
            search_tt().store(board.get_hash_key(), Move(), stand_pat, 0, HASH_BETA, ply);
            return stand_pat;
        }
        if (stand_pat > alpha) alpha = stand_pat;
    }
    
    MoveList move_list;
    if (in_check) generate_evasion_moves(board, move_list);
    else generate_noisy_moves(board, move_list);
    MovePicker move_picker(board, move_list, tt_move, ply);
    
    LegalityMasks masks = board.get_legality_masks();
    int moves_searched = 0;
    Move best_move;

    Move move;
    while (move_picker.next(move)) {
        
        // Outside check, quiescence includes captures and promotions. In
        // check, every legal evasion (including quiet king/block moves) is
        // mandatory and none may be pruned by stand-pat, futility, or SEE.
        if (!in_check && !move.is_capture() && !move.is_promotion()) {
            continue;
        }

        // Safe QS Futility Pruning
        if (Search::mate_limit == 0 && !in_check && moves_searched >= 2 && !move.is_promotion()
            && std::abs(alpha) < MATE_SCORE - MAX_PLY) {
            Piece captured = board.get_piece_at(move.to());
            int cap_type = (captured != NO_PIECE) ? (captured % 6) : PAWN;
            const int piece_values[6] = { 100, 320, 330, 500, 900, 20000 };
            if (stand_pat + 350 + piece_values[cap_type] <= alpha) {
                continue;
            }
        }

        // Static Exchange Evaluation: prune losing captures
        if (Search::mate_limit == 0 && !in_check && board.see(move) < 0) {
            continue;
        }
        
        if (!board.is_move_legal(move, masks)) {
            continue;
        }
        board.make_move(move, true);
        search_tt().prefetch(board.get_hash_key());
        moves_searched++;
        
        int score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmake_move(move);
        
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        
        if (score >= beta) {
            search_tt().store(board.get_hash_key(), move, score, 0, HASH_BETA, ply);
            return score;
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    if (in_check && moves_searched == 0)
        return -MATE_SCORE + ply;

    const uint8_t flag = best_score <= alpha_orig ? HASH_ALPHA : HASH_EXACT;
    search_tt().store(board.get_hash_key(), best_move, best_score, 0, flag, ply);
    return best_score;
}

namespace Search {
    std::atomic<uint64_t> start_time{0};
    uint64_t target_time = 0;
    uint64_t soft_limit = 0;
    uint64_t hard_limit = 0;
    std::atomic<bool> b_abort{false};
    std::atomic<bool> pondering{false};
    std::atomic<int> active_helpers{0};
    uint64_t time_check_mask = 1023; // Default check every 1024 nodes
    uint64_t node_limit = 0;
    int mate_limit = 0;

    int num_threads = 1;
    ThreadStats thread_stats[MAX_THREADS];

    int RFP_Margin = 70;
    int LMR_Constant_Scaled = 218;
    int NMP_Base = 3;
    int NMP_Divisor = 7;
    int Aspiration_Delta = 18;
    int History_Threshold = 15576;
    int Move_Overhead = 30;
    int SyzygyProbeDepth = 1;
    bool SyzygyProbeLimit = true;
    bool Syzygy50MoveRule = true;
    int LMR_History_Divisor = 7302;
    int Contempt = 0;
    int SEE_Pruning_Depth = 0;
    bool Ponder = false;
    int MultiPV = 1;
    bool UCI_ShowWDL = false;
    bool UCI_AnalyseMode = false;

    void init_search_tables() {
        for (int d = 1; d < 64; d++) {
            for (int m = 1; m < 64; m++) {
                lmr_table[d][m] = static_cast<int>(0.5 + log(d) * log(m) / (static_cast<double>(LMR_Constant_Scaled) / 100.0));
            }
        }
    }

    void allocate_time(int time_left, int increment, int moves_to_go) {
        int usable_time = time_left - Move_Overhead;
        if (usable_time <= 0) {
            usable_time = 10;
        }

        const int horizon = moves_to_go > 0 ? std::clamp(moves_to_go, 1, 50) : 40;
        soft_limit = static_cast<uint64_t>((usable_time / horizon) + increment);

        int calculated_multiplier = static_cast<int>(soft_limit * 2.5);
        int absolute_max_cap = static_cast<int>(usable_time * 0.40);
        hard_limit = static_cast<uint64_t>(std::min(calculated_multiplier, absolute_max_cap));

        if (soft_limit < 15) soft_limit = 15;
        if (hard_limit < 25) hard_limit = 25;
    }

    // Compute time controls from clock parameters (called before thread launch)
    void compute_time_controls(Color side, const Limits& limits) {
        soft_limit = 0;
        hard_limit = 0;
        target_time = 0;
        node_limit = limits.nodes;
        mate_limit = limits.mate;

        if (limits.infinite) {
            // Infinite analysis is still allowed to have an explicit depth or
            // node cap, but it never consumes the supplied clock values.
        } else if (limits.movetime > 0) {
            int usable_movetime = limits.movetime - Move_Overhead;
            if (usable_movetime < 10) {
                usable_movetime = std::max(5, limits.movetime / 2);
            }
            hard_limit = static_cast<uint64_t>(std::min(usable_movetime, 55000));
            soft_limit = hard_limit;
            target_time = start_time.load(std::memory_order_relaxed) + hard_limit;
        } else {
            int my_time = (side == WHITE) ? limits.wtime : limits.btime;
            int my_inc = (side == WHITE) ? limits.winc : limits.binc;

            if (my_time > 0) {
                allocate_time(my_time, my_inc, limits.movestogo);

                int safety_buffer = Move_Overhead + 10;
                if (hard_limit + safety_buffer > static_cast<uint64_t>(my_time)) {
                    if (my_time > safety_buffer) {
                        hard_limit = my_time - safety_buffer;
                    } else {
                        hard_limit = std::max(5, my_time / 2);
                    }
                }

                if (hard_limit > static_cast<uint64_t>(my_time)) {
                    hard_limit = my_time;
                }
                target_time = start_time.load(std::memory_order_relaxed) + hard_limit;
            } else if (my_time == 0) {
                soft_limit = 10;
                hard_limit = 20;
                target_time = start_time.load(std::memory_order_relaxed) + hard_limit;
            } else {
                soft_limit = 0;
                hard_limit = 0;
                target_time = 0;
            }
        }

        if (node_limit > 0) {
            // Small node searches need fine granularity; larger searches can
            // amortize the relaxed-atomic publication cost.
            time_check_mask = node_limit <= 1024 ? 0
                            : node_limit <= 16384 ? 15
                            : 127;
        } else if (hard_limit > 0) {
            if (hard_limit < 100) {
                time_check_mask = 127;
            } else if (hard_limit < 500) {
                time_check_mask = 511;
            } else {
                time_check_mask = 1023;
            }
        } else {
            time_check_mask = 1023;
        }
    }

    void ponder_hit() {
        if (pondering.exchange(false, std::memory_order_acq_rel)) {
            start_time.store(get_time_ms(), std::memory_order_release);
        }
    }

    void reset_root_node_accounting(const Board& board) {
        root_node_stat_count = 0;
        MoveList moves;
        generate_pseudo_legal_moves(board, moves);
        const LegalityMasks masks = board.get_legality_masks();
        for (int i = 0; i < moves.count && root_node_stat_count < 256; ++i) {
            if (!board.is_move_legal(moves.moves[i], masks)) continue;
            root_node_stats[root_node_stat_count].move = moves.moves[i];
            for (int t = 0; t < num_threads; ++t)
                root_node_counts[t][root_node_stat_count] = 0;
            ++root_node_stat_count;
        }
    }

    void set_root_moves(const std::vector<Move>& moves, bool restricted) {
        root_moves_restricted = restricted;
        root_allowed_count = std::min(static_cast<int>(moves.size()), 256);
        for (int i = 0; i < root_allowed_count; ++i)
            root_allowed_moves[i] = moves[i];
    }

    void set_thread_tt(TranspositionTable* table) {
        thread_tt = table ? table : &tt;
    }

    void reset_worker_search_state(Color root_side, int worker_id) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = std::clamp(worker_id, 0, MAX_THREADS - 1);
        root_color = root_side;
        root_excluded_count = 0;
        root_search_best_move = Move();
        currmove_report_depth = -1;
        reported_currmove_count = 0;
        root_accounting_enabled = false;
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i)
            search_stack[i] = SearchStack{};
        thread_stats[active_thread_id].nodes.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].tbhits.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].seldepth.store(0, std::memory_order_relaxed);
    }

    void finish_helpers() {
        b_abort.store(true, std::memory_order_release);
        while (active_helpers.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
    }

    // Master search control loop with Iterative Deepening (main thread only)
    void search_position(Board& board, int max_depth) {
        // These counters describe the whole search, not only the latest
        // iterative-deepening pass.  Reset them once at the entry point.
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        root_accounting_enabled = true;
        root_excluded_count = 0;
        root_search_best_move = Move();
        currmove_report_depth = -1;
        reported_currmove_count = 0;
        search_tt().new_search();

        // Reset killer moves and history table at start of search
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i) {
            search_stack[i] = SearchStack{};
        }
        // Count legal moves at the root
        int num_legal_moves = 0;
        MoveList root_list;
        generate_pseudo_legal_moves(board, root_list);
        for (int i = 0; i < root_list.count; i++) {
            if (root_move_is_allowed(root_list.moves[i]) && board.make_move(root_list.moves[i])) {
                num_legal_moves++;
                board.unmake_move(root_list.moves[i]);
            }
        }

        if (num_legal_moves == 0) {
            while (pondering.load(std::memory_order_relaxed)
                   && !b_abort.load(std::memory_order_relaxed))
                std::this_thread::yield();
            finish_helpers();
            std::cout << "bestmove 0000\n" << std::flush;
            return;
        }

        // Syzygy DTZ Probing at the root
        int num_pieces = count_bits(board.get_occupancy(BOTH));
        if (MultiPV == 1 && TB_LARGEST > 0 && num_pieces <= (int)TB_LARGEST && board.get_castling_rights() == 0) {
            unsigned results[TB_MAX_MOVES];
            U64 white = board.get_occupancy(WHITE);
            U64 black = board.get_occupancy(BLACK);
            U64 kings = board.get_pieces(WHITE, KING) | board.get_pieces(BLACK, KING);
            U64 queens = board.get_pieces(WHITE, QUEEN) | board.get_pieces(BLACK, QUEEN);
            U64 rooks = board.get_pieces(WHITE, ROOK) | board.get_pieces(BLACK, ROOK);
            U64 bishops = board.get_pieces(WHITE, BISHOP) | board.get_pieces(BLACK, BISHOP);
            U64 knights = board.get_pieces(WHITE, KNIGHT) | board.get_pieces(BLACK, KNIGHT);
            U64 pawns = board.get_pieces(WHITE, PAWN) | board.get_pieces(BLACK, PAWN);
            unsigned ep = board.get_en_passant_square();
            if (ep == SQ_NONE) ep = 0;
            bool turn = (board.get_side_to_move() == WHITE);
            unsigned rule50 = Syzygy50MoveRule ? board.get_halfmove_clock() : 0;
            
            unsigned res = tb_probe_root(white, black, kings, queens, rooks, bishops, knights, pawns, rule50, 0, ep, turn, results);
            if (res != TB_RESULT_FAILED && res != TB_RESULT_CHECKMATE && res != TB_RESULT_STALEMATE) {
                Move tb_move = fathom_to_coco_move(board, res);
                bool is_legal = false;
                LegalityMasks masks = board.get_legality_masks();
                for (int i = 0; i < root_list.count; i++) {
                    if (root_list.moves[i] == tb_move && root_move_is_allowed(tb_move)
                        && board.is_move_legal(tb_move, masks)) {
                        is_legal = true;
                        break;
                    }
                }
                
                if (is_legal) {
                    thread_stats[0].tbhits.fetch_add(1, std::memory_order_relaxed);
                    unsigned wdl = TB_GET_WDL(res);
                    unsigned dtz = TB_GET_DTZ(res);
                    int tb_score = 0;
                    if (wdl == TB_WIN) {
                        tb_score = VALUE_TB - dtz - rule50;
                    } else if (wdl == TB_CURSED_WIN) {
                        tb_score = 1;
                    } else if (wdl == TB_DRAW) {
                        tb_score = 0;
                    } else if (wdl == TB_BLESSED_LOSS) {
                        tb_score = -1;
                    } else if (wdl == TB_LOSS) {
                        tb_score = -VALUE_TB + dtz + rule50;
                    }
                    
                    while (pondering.load(std::memory_order_relaxed)
                           && !b_abort.load(std::memory_order_relaxed))
                        std::this_thread::yield();
                    finish_helpers();
                    std::cout << "info depth 1 ";
                    print_uci_score(tb_score);
                    std::cout << " nodes 1 nps 1000 tbhits 1 pv "
                              << move_to_str(tb_move) << "\n";
                    std::cout << "bestmove " << move_to_str(tb_move) << "\n";
                    std::cout << std::flush;
                    return;
                }
            }
        }
        
        root_color = board.get_side_to_move();

        if (MultiPV > 1) {
            struct RootLine {
                Move move;
                int score = -INFINITY_SCORE;
                uint64_t root_nodes = 0;
                std::vector<Move> pv;
            };

            const int requested_lines = std::max(1, std::min(MultiPV, num_legal_moves));
            std::vector<RootLine> completed_lines;
            const int target_depth = max_depth;

            for (int current_depth = 1; current_depth <= target_depth; ++current_depth) {
                std::vector<RootLine> current_lines;
                root_excluded_count = 0;

                for (int pv_index = 0; pv_index < requested_lines; ++pv_index) {
                    check_time();
                    if (b_abort.load(std::memory_order_relaxed)) break;

                    root_search_best_move = Move();
                    const int score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE,
                                                 current_depth, 0, NodeType::PV, false);
                    if (b_abort.load(std::memory_order_relaxed)) break;

                    const Move selected = root_search_best_move;
                    if (selected.is_none() || root_move_is_excluded(selected)) break;

                    RootLine line;
                    line.move = selected;
                    line.score = score;
                    line.root_nodes = root_nodes_for_move(selected);
                    line.pv.push_back(selected);

                    if (board.make_move(selected)) {
                        Move tail[63];
                        const int tail_length = get_pv(board, tail, 63);
                        for (int i = 0; i < tail_length; ++i)
                            line.pv.push_back(tail[i]);
                        board.unmake_move(selected);
                    }

                    current_lines.push_back(std::move(line));
                    root_excluded_moves[root_excluded_count++] = selected;
                }

                root_excluded_count = 0;
                if (b_abort.load(std::memory_order_relaxed) || current_lines.empty()) break;

                std::stable_sort(current_lines.begin(), current_lines.end(),
                    [](const RootLine& lhs, const RootLine& rhs) {
                        return lhs.score > rhs.score;
                    });
                completed_lines = std::move(current_lines);

                thread_stats[0].nodes.store(nodes_visited, std::memory_order_relaxed);
                thread_stats[0].seldepth.store(max_ply_reached, std::memory_order_relaxed);

                uint64_t total_nodes = 0;
                int total_seldepth = max_ply_reached;
                for (int t = 0; t < num_threads; ++t) {
                    total_nodes += thread_stats[t].nodes.load(std::memory_order_relaxed);
                    total_seldepth = std::max(total_seldepth,
                        thread_stats[t].seldepth.load(std::memory_order_relaxed));
                }

                const uint64_t elapsed = get_time_ms()
                    - start_time.load(std::memory_order_relaxed);
                const uint64_t nps = elapsed > 0 ? total_nodes * 1000 / elapsed
                                                  : total_nodes * 1000;

                for (int i = 0; i < static_cast<int>(completed_lines.size()); ++i) {
                    const RootLine& line = completed_lines[i];
                    std::cout << "info depth " << current_depth
                              << " seldepth " << total_seldepth
                              << " multipv " << (i + 1) << " ";

                    print_uci_score(line.score);

                    std::cout << " nodes " << total_nodes << " time " << elapsed
                              << " nps " << nps << " hashfull " << search_tt().hashfull()
                              << " tbhits " << aggregate_tbhits()
                              << " pv";
                    for (Move pv_move : line.pv)
                        std::cout << " " << move_to_str(pv_move);
                    std::cout << "\n";
                }

                if (num_legal_moves == 1) break;
                if (!completed_lines.empty()
                    && mate_within_limit(completed_lines.front().score)) break;
                if (!pondering.load(std::memory_order_relaxed)
                    && soft_limit != 0 && elapsed > soft_limit) break;
            }

            Move best_move;
            Move ponder_move;
            if (!completed_lines.empty()) {
                best_move = completed_lines.front().move;
                if (completed_lines.front().pv.size() > 1)
                    ponder_move = completed_lines.front().pv[1];
            } else {
                LegalityMasks masks = board.get_legality_masks();
                for (int i = 0; i < root_list.count; ++i) {
                    if (root_move_is_allowed(root_list.moves[i])
                        && board.is_move_legal(root_list.moves[i], masks)) {
                        best_move = root_list.moves[i];
                        break;
                    }
                }
            }

            while (pondering.load(std::memory_order_relaxed)
                   && !b_abort.load(std::memory_order_relaxed))
                std::this_thread::yield();
            finish_helpers();

            std::cout << "bestmove " << move_to_str(best_move);
            if (Ponder && !ponder_move.is_none())
                std::cout << " ponder " << move_to_str(ponder_move);
            std::cout << "\n" << std::flush;
            return;
        }
        
        Move best_move;
        Move last_completed_best_move;
        Move last_completed_ponder_move;
        Move previous_best_move = Move();
        int best_move_stability_count = 0;
        
        int target_depth = max_depth;
        int last_score = 0;

        // Iterative Deepening loop
        for (int current_depth = 1; current_depth <= target_depth; ++current_depth) {
            int score = 0;

            if (current_depth < 3) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, NodeType::PV, false);
            } else {
                int delta = Aspiration_Delta;
                int alpha = std::max(last_score - delta, -INFINITY_SCORE);
                int beta = std::min(last_score + delta, INFINITY_SCORE);
                
                while (true) {
                    check_time();
                    if (b_abort.load(std::memory_order_relaxed)) {
                        break;
                    }

                    score = alpha_beta(board, alpha, beta, current_depth, 0, NodeType::PV, false);

                    if (b_abort.load(std::memory_order_relaxed)) {
                        break;
                    }
                    
                    if (score <= alpha) {
                        alpha = std::max(alpha - delta, -INFINITY_SCORE);
                        delta += delta / 2;
                    } else if (score >= beta) {
                        beta = std::min(beta + delta, INFINITY_SCORE);
                        delta += delta / 2;
                    } else {
                        // Score is within bounds, search succeeded
                        break;
                    }
                }
            }
            
            // Check if we aborted mid-depth
            if (b_abort.load(std::memory_order_relaxed)) {
                break;
            }
            
            // Single legal move optimization: immediately break after completing depth 1
            if (num_legal_moves == 1 && current_depth >= 1) {
                break;
            }
            
            // Probe TT for the best move of the completed depth
            int temp_score;
            Move best_move_depth;
            search_tt().probe(board.get_hash_key(), temp_score, best_move_depth, current_depth, -INFINITY_SCORE, INFINITY_SCORE, 0);
            
            if (!best_move_depth.is_none()) {
                last_completed_best_move = best_move_depth;
            }
            
            // Extract the PV from the TT cache
            Move pv[64];
            int pv_len = get_pv(board, pv, 64);
            if (pv_len > 0 && last_completed_best_move.is_none()) {
                last_completed_best_move = pv[0];
            }
            if (pv_len > 1) {
                last_completed_ponder_move = pv[1];
            }

            // Track stability of the best move
            Move current_best_move = last_completed_best_move;
            if (current_depth > 1) {
                if (!current_best_move.is_none() && !previous_best_move.is_none() && current_best_move == previous_best_move) {
                    best_move_stability_count++;
                } else {
                    best_move_stability_count = 0;
                }
            }
            previous_best_move = current_best_move;

            // Calculate dynamic time multiplier
            double time_multiplier = 1.0;
            if (current_depth >= 5) {
                if (best_move_stability_count >= 3) {
                    time_multiplier *= 0.65; // Best move highly stable: reduce time by 35%
                } else if (best_move_stability_count >= 2) {
                    time_multiplier *= 0.80; // Best move stable: reduce time by 20%
                } else if (best_move_stability_count == 0) {
                    time_multiplier *= 1.40; // Best move changed: think 40% longer
                }

                if (score < last_score - 30) {
                    time_multiplier *= 1.35; // Score dropping: think 35% longer
                }
            }

            uint64_t adjusted_soft_limit = soft_limit;
            if (soft_limit > 0) {
                adjusted_soft_limit = static_cast<uint64_t>(soft_limit * time_multiplier);
                // Prevent over-pruning or exceeding hard limit bounds
                adjusted_soft_limit = std::max(adjusted_soft_limit, static_cast<uint64_t>(soft_limit * 0.3));
                adjusted_soft_limit = std::min(adjusted_soft_limit, hard_limit);
            }

            last_score = score;

            // Update thread 0 stats
            thread_stats[0].nodes.store(nodes_visited, std::memory_order_relaxed);
            thread_stats[0].seldepth.store(max_ply_reached, std::memory_order_relaxed);

            // Aggregate metrics across all threads
            uint64_t total_nodes = 0;
            int total_seldepth = max_ply_reached;
            for (int t = 0; t < num_threads; t++) {
                total_nodes += thread_stats[t].nodes.load(std::memory_order_relaxed);
                total_seldepth = std::max(
                    total_seldepth,
                    thread_stats[t].seldepth.load(std::memory_order_relaxed));
            }

            uint64_t elapsed = get_time_ms() - start_time.load(std::memory_order_relaxed);
            uint64_t nps = elapsed > 0 ? (total_nodes * 1000) / elapsed : total_nodes * 1000;

            // Print standardized UCI info line
            std::cout << "info depth " << current_depth << " seldepth " << total_seldepth << " ";

            print_uci_score(score);

            std::cout << " nodes " << total_nodes << " time " << elapsed
                      << " nps " << nps << " hashfull " << search_tt().hashfull()
                      << " tbhits " << aggregate_tbhits() << " pv";

            for (int i = 0; i < pv_len; i++) {
                std::cout << " " << move_to_str(pv[i]);
            }
            std::cout << "\n";

            if (mate_within_limit(score)) break;

            if (!pondering.load(std::memory_order_relaxed)
                && adjusted_soft_limit != 0 && elapsed > adjusted_soft_limit) {
                break;
            }
        }
        
        // Fallback: if search aborted before completion of depth 1, select first legal move
        if (last_completed_best_move.is_none()) {
            MoveList list;
            generate_pseudo_legal_moves(board, list);
            for (int i = 0; i < list.count; i++) {
                if (root_move_is_allowed(list.moves[i]) && board.make_move(list.moves[i])) {
                    last_completed_best_move = list.moves[i];
                    board.unmake_move(list.moves[i]);
                    break;
                }
            }
        }
        
        while (pondering.load(std::memory_order_relaxed)
               && !b_abort.load(std::memory_order_relaxed))
            std::this_thread::yield();
        finish_helpers();

        if (node_limit != 0) {
            thread_stats[0].nodes.store(nodes_visited, std::memory_order_relaxed);
            thread_stats[0].seldepth.store(max_ply_reached, std::memory_order_relaxed);

            uint64_t final_nodes = 0;
            int final_seldepth = max_ply_reached;
            for (int t = 0; t < num_threads; ++t) {
                final_nodes += thread_stats[t].nodes.load(std::memory_order_relaxed);
                final_seldepth = std::max(
                    final_seldepth,
                    thread_stats[t].seldepth.load(std::memory_order_relaxed));
            }
            const uint64_t elapsed = get_time_ms() - start_time.load(std::memory_order_relaxed);
            const uint64_t nps = elapsed > 0 ? final_nodes * 1000 / elapsed : final_nodes * 1000;
            std::cout << "info seldepth " << final_seldepth << " nodes " << final_nodes
                      << " time " << elapsed << " nps " << nps
                      << " hashfull " << search_tt().hashfull()
                      << " tbhits " << aggregate_tbhits() << "\n";
        }

        // Report final best move designation to GUI
        std::cout << "bestmove " << move_to_str(last_completed_best_move);
        if (Ponder && !last_completed_ponder_move.is_none())
            std::cout << " ponder " << move_to_str(last_completed_ponder_move);
        std::cout << "\n";
        std::cout << std::flush;
    }

    // Helper thread search entry point (threads 1..N-1)
    void search_helper(Board& board, int max_depth, int thread_id) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = thread_id;
        root_accounting_enabled = true;
        if (b_abort.load(std::memory_order_relaxed)) {
            active_helpers.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        // Clear thread-local killer moves and history table
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i) {
            search_stack[i] = SearchStack{};
        }

        root_color = board.get_side_to_move();

        int target_depth = max_depth > 0 ? max_depth : MAX_PLY;
        // Diverse depth staggering across helper threads (spreads them across offsets 0, 1, 2)
        int start_depth = 1 + (thread_id % 3);
        int last_score = 0;

        for (int current_depth = start_depth; current_depth <= target_depth; ++current_depth) {
            int score = 0;

            if (current_depth < 3) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, NodeType::PV, false);
            } else {
                // Vary aspiration delta by thread to avoid synchronized fail-highs/lows
                int delta = Aspiration_Delta + (thread_id % 4) * 4;
                int alpha = std::max(last_score - delta, -INFINITY_SCORE);
                int beta = std::min(last_score + delta, INFINITY_SCORE);

                while (true) {
                    if (b_abort.load(std::memory_order_relaxed)) break;

                    score = alpha_beta(board, alpha, beta, current_depth, 0, NodeType::PV, false);

                    if (b_abort.load(std::memory_order_relaxed)) break;

                    if (score <= alpha) {
                        alpha = std::max(alpha - delta, -INFINITY_SCORE);
                        delta += delta / 2;
                    } else if (score >= beta) {
                        beta = std::min(beta + delta, INFINITY_SCORE);
                        delta += delta / 2;
                    } else {
                        break;
                    }
                }
            }

            // Update per-thread stats
            thread_stats[thread_id].nodes.store(nodes_visited, std::memory_order_relaxed);
            thread_stats[thread_id].seldepth.store(max_ply_reached, std::memory_order_relaxed);

            if (b_abort.load(std::memory_order_relaxed)) break;

            last_score = score;
        }

        thread_stats[thread_id].nodes.store(nodes_visited, std::memory_order_relaxed);
        thread_stats[thread_id].seldepth.store(max_ply_reached, std::memory_order_relaxed);
        active_helpers.fetch_sub(1, std::memory_order_release);
    }

#ifdef COCO_TESTING
    int test_quiescence_window(Board& board, int alpha, int beta) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        root_color = board.get_side_to_move();
        b_abort.store(false, std::memory_order_relaxed);
        hard_limit = 0;
        node_limit = 0;
        return ::quiescence(board, alpha, beta, 0);
    }

    int test_alpha_beta_window(Board& board, int alpha, int beta, int depth) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        start_time.store(get_time_ms(), std::memory_order_relaxed);
        root_color = board.get_side_to_move();
        root_accounting_enabled = true;
        b_abort.store(false, std::memory_order_relaxed);
        hard_limit = 0;
        node_limit = 0;
        return ::alpha_beta(board, alpha, beta, depth, 0, NodeType::NON_PV);
    }

    int test_alpha_beta_pv(Board& board, int alpha, int beta, int depth) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        start_time.store(get_time_ms(), std::memory_order_relaxed);
        root_color = board.get_side_to_move();
        root_accounting_enabled = true;
        reset_root_node_accounting(board);
        set_root_moves({}, false);
        b_abort.store(false, std::memory_order_relaxed);
        hard_limit = 0;
        node_limit = 0;
        return ::alpha_beta(board, alpha, beta, depth, 0, NodeType::PV);
    }

    NmpTestResult test_nmp_window(Board& board, int alpha, int beta, int depth,
                                  NodeType node_type) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        start_time.store(get_time_ms(), std::memory_order_relaxed);
        root_color = board.get_side_to_move();
        nmp_test_attempts = 0;
        nmp_test_cutoffs = 0;
        b_abort.store(false, std::memory_order_relaxed);
        hard_limit = 0;
        node_limit = 0;
        const int score = ::alpha_beta(board, alpha, beta, depth, 0, node_type);
        return {score, nmp_test_attempts, nmp_test_cutoffs};
    }

    uint64_t test_root_nodes_total() {
        uint64_t total = 0;
        for (int t = 0; t < num_threads; ++t)
            for (int i = 0; i < root_node_stat_count; ++i)
                total += root_node_counts[t][i];
        return total;
    }

    uint64_t test_root_nodes_for(Move move) {
        return root_nodes_for_move(move);
    }

    int test_get_pv(Board& board, Move* pv, int max_depth) {
        return get_pv(board, pv, max_depth);
    }

    void test_score_to_wdl(int score, int& win, int& draw, int& loss) {
        const WdlPermille wdl = score_to_wdl(score);
        win = wdl.win;
        draw = wdl.draw;
        loss = wdl.loss;
    }
#endif
}
