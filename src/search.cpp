#include "search.h"
#include "ravager_eval.h"

#include "types.h"
#include "movegen.h"
#include "tt.h"
#include "evaluate.h"
#include "search_policy.h"
#include "tbprobe.h"
#include <iostream>

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

thread_local uint64_t nodes_visited = 0;
thread_local int active_thread_id = 0;

thread_local int max_ply_reached = 0;
thread_local Move root_excluded_moves[256];
thread_local int root_excluded_count = 0;
thread_local Move root_search_best_move;
thread_local Move root_first_search_move;
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
std::atomic<uint64_t> root_node_counts[MAX_THREADS][256]{};
int root_node_stat_count = 0;
Move root_allowed_moves[256];
int root_allowed_count = 0;
bool root_moves_restricted = false;

inline void record_root_nodes(Move move, uint64_t nodes) {
    if (!root_accounting_enabled) return;
    for (int i = 0; i < root_node_stat_count; ++i) {
        if (root_node_stats[i].move == move) {
            root_node_counts[active_thread_id][i].fetch_add(
                nodes, std::memory_order_relaxed);
            return;
        }
    }
}

inline uint64_t root_nodes_for_move(Move move) {
    for (int i = 0; i < root_node_stat_count; ++i) {
        if (root_node_stats[i].move == move)
            return root_node_counts[0][i].load(std::memory_order_relaxed);
    }
    return 0;
}

inline uint64_t root_nodes_total() {
    uint64_t total = 0;
    for (int move_index = 0; move_index < root_node_stat_count; ++move_index)
        for (int thread = 0; thread < Search::num_threads; ++thread)
            total += root_node_counts[thread][move_index].load(
                std::memory_order_relaxed);
    return total;
}

inline double root_node_fraction_multiplier(uint64_t best_nodes,
                                            uint64_t total_nodes) {
    if (total_nodes == 0 || best_nodes > total_nodes) return 1.0;
    const double fraction = static_cast<double>(best_nodes)
        / static_cast<double>(total_nodes);
    if (fraction >= 0.60) return 0.85;
    if (fraction <= 0.20) return 1.15;
    return 1.0;
}
#ifdef COCO_TESTING
thread_local int nmp_test_attempts = 0;
thread_local int nmp_test_cutoffs = 0;
thread_local int probcut_test_attempts = 0;
thread_local int probcut_test_cutoffs = 0;
thread_local uint64_t probcut_test_nodes = 0;
thread_local int probcut_test_last_score = -INFINITY_SCORE;
#endif

inline uint64_t get_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

inline void check_time(bool root = false) {
    if ((nodes_visited & Search::time_check_mask) != 0
        && !(root && Search::hard_limit != 0))
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

thread_local Move killer_moves[MAX_PLY][2];
thread_local Move explicit_pv[MAX_PLY][MAX_PLY];
thread_local int explicit_pv_length[MAX_PLY];
thread_local Color root_color = WHITE;
thread_local int16_t history_table[2][2][2][7][64]; // [color][threat_from][threat_to][piece_type][to_square] - size 7 to prevent out-of-bounds on NO_PIECE_TYPE
int lmr_table[64][64];

using Search::NodeType;

int get_pv(Board& board, Move* pv_array, int max_pv_depth);
int get_pv_impl(Board& board, Move* pv_array, int max_pv_depth,
                bool include_bound_moves);

inline void reset_explicit_pv(int ply) {
    if (ply >= 0 && ply < MAX_PLY)
        explicit_pv_length[ply] = ply;
}

inline void update_explicit_pv(int ply, Move move) {
    if (ply < 0 || ply >= MAX_PLY || move.is_none()) return;
    explicit_pv[ply][ply] = move;
    int end = ply + 1;
    if (ply + 1 < MAX_PLY)
        end = std::clamp(explicit_pv_length[ply + 1], ply + 1, MAX_PLY);
    for (int index = ply + 1; index < end; ++index)
        explicit_pv[ply][index] = explicit_pv[ply + 1][index];
    explicit_pv_length[ply] = end;
}

int copy_explicit_pv(Move* output, int max_depth) {
    if (!output || max_depth <= 0) return 0;
    const int length = std::clamp(explicit_pv_length[0], 0,
                                  std::min(max_depth, MAX_PLY));
    for (int index = 0; index < length; ++index)
        output[index] = explicit_pv[0][index];
    return length;
}

static bool pv_draw_boundary(const Board& board) {
    return board.get_halfmove_clock() >= 100 || board.is_repetition();
}

int complete_explicit_pv_from_tt(Board& board, Move* pv, int length,
                                 int max_depth, bool include_bound_moves) {
    int replayed = 0;
    bool reached_draw = false;
    while (replayed < length) {
        if (!board.make_move(pv[replayed], true)) {
            break;
        }
        ++replayed;
        reached_draw = pv_draw_boundary(board);
        if (reached_draw) break;
    }

    length = replayed;
    if (!reached_draw && length < max_depth) {
        Move tail[MAX_PLY];
        const int tail_length = get_pv_impl(
            board, tail, max_depth - length, include_bound_moves);
        for (int index = 0; index < tail_length; ++index)
            pv[length + index] = tail[index];
        length += tail_length;
    }

    for (int index = replayed - 1; index >= 0; --index)
        board.unmake_move(pv[index]);
    return length;
}

struct SearchStack {
    int piece = -1;
    int to_sq = -1;
    int static_eval = INFINITY_SCORE;
    Move current_move;
#ifdef COCO_CORRHIST_PROFILE
    int16_t corr_profile_value[3]{};
#endif
};
thread_local SearchStack search_stack[MAX_PLY + 4];
thread_local int16_t cont_history[2][7][64][7][64];
// Capture history also buckets by whether the destination is threatened.
thread_local int16_t capture_history[12][64][2][6];

#ifdef COCO_CORRHIST_PROFILE
// Shadow tables measure size and lifetime effects without affecting search.
struct CorrHistProfileStats {
    uint64_t lookups = 0;
    uint64_t occupied_hits = 0;
    uint64_t exact_key_hits = 0;
    uint64_t collisions = 0;
    uint64_t cross_search_hits = 0;
    uint64_t updates = 0;
    uint64_t informative_predictions = 0;
    uint64_t sign_matches = 0;
    uint64_t absolute_error_before = 0;
    uint64_t absolute_error_after = 0;
    uint64_t saturations = 0;
};

struct CorrHistProfileEntry {
    int16_t value = 0;
    uint16_t key = 0;
    uint32_t last_search = 0;
    bool occupied = false;
};

template <size_t Size>
struct CorrHistProfileTable {
    static_assert((Size & (Size - 1)) == 0);
    CorrHistProfileEntry entries[2][Size]{};
    CorrHistProfileStats stats{};

    void begin_search(bool clear_entries) {
        stats = CorrHistProfileStats{};
        if (clear_entries)
            std::memset(entries, 0, sizeof(entries));
    }

    int16_t lookup(Color side, uint16_t key, uint32_t search_generation) {
        CorrHistProfileEntry& entry = entries[side][key & (Size - 1)];
        ++stats.lookups;
        if (!entry.occupied)
            return 0;
        ++stats.occupied_hits;
        if (entry.key == key)
            ++stats.exact_key_hits;
        else
            ++stats.collisions;
        if (entry.last_search != 0 && entry.last_search < search_generation)
            ++stats.cross_search_hits;
        return entry.value;
    }

    void update(Color side, uint16_t key, uint32_t search_generation,
                int16_t prediction, int raw_eval, int result, int depth) {
        const int error = result - raw_eval;
        const int correction = prediction * 29 / 256;
        ++stats.updates;
        stats.absolute_error_before += std::abs(error);
        stats.absolute_error_after += std::abs(error - correction);
        if (prediction != 0 && error != 0) {
            ++stats.informative_predictions;
            if ((prediction > 0) == (error > 0))
                ++stats.sign_matches;
        }

        const int bonus = std::clamp(error * depth / 8, -256, 256);
        CorrHistProfileEntry& entry = entries[side][key & (Size - 1)];
        int value = entry.value;
        value += bonus - value * std::abs(bonus) / 1024;
        entry.value = static_cast<int16_t>(std::clamp(value, -1024, 1024));
        entry.key = key;
        entry.last_search = search_generation;
        entry.occupied = true;
        if (std::abs(entry.value) >= 1000)
            ++stats.saturations;
    }
};

// The UCI layer creates fresh OS search threads for every `go`. Keeping the
// persistent shadow outside thread-local storage is therefore essential to
// measure actual between-move lifetime. Only worker 0 touches these objects.
CorrHistProfileTable<16384> corr_profile_old_reset;
CorrHistProfileTable<4096> corr_profile_small_reset;
CorrHistProfileTable<4096> corr_profile_small_persistent;
uint32_t corr_profile_search_generation = 0;

inline void corrhist_profile_begin_search() {
    ++corr_profile_search_generation;
    corr_profile_old_reset.begin_search(true);
    corr_profile_small_reset.begin_search(true);
    corr_profile_small_persistent.begin_search(false);
}

inline void corrhist_profile_lookup(const Board& board, SearchStack& stack) {
    if (active_thread_id != 0)
        return;
    const Color side = board.get_side_to_move();
    const uint16_t key = board.get_pawn_key();
    stack.corr_profile_value[0] = corr_profile_old_reset.lookup(
        side, key, corr_profile_search_generation);
    stack.corr_profile_value[1] = corr_profile_small_reset.lookup(
        side, key, corr_profile_search_generation);
    stack.corr_profile_value[2] = corr_profile_small_persistent.lookup(
        side, key, corr_profile_search_generation);
}

inline void corrhist_profile_update(const Board& board, const SearchStack& stack,
                                    int raw_eval, int result, int depth,
                                    bool in_check, Move best_move,
                                    uint8_t bound, Move excluded_move) {
    if (active_thread_id != 0 || in_check || !excluded_move.is_none()
        || best_move.is_none()
        || best_move.is_capture() || best_move.is_promotion()
        || std::abs(result) >= MATE_THRESHOLD)
        return;
    if ((bound == HASH_BETA && result <= raw_eval)
        || (bound == HASH_ALPHA && result >= raw_eval))
        return;

    const Color side = board.get_side_to_move();
    const uint16_t key = board.get_pawn_key();
    corr_profile_old_reset.update(side, key, corr_profile_search_generation,
                                  stack.corr_profile_value[0], raw_eval,
                                  result, depth);
    corr_profile_small_reset.update(side, key, corr_profile_search_generation,
                                    stack.corr_profile_value[1], raw_eval,
                                    result, depth);
    corr_profile_small_persistent.update(
        side, key, corr_profile_search_generation,
        stack.corr_profile_value[2], raw_eval, result, depth);
}

template <size_t Size>
void print_corrhist_profile_table(const char* name,
                                  const CorrHistProfileTable<Size>& table) {
    const CorrHistProfileStats& stats = table.stats;
    std::cout << "info string corrhist_profile " << name
              << " lookups=" << stats.lookups
              << " occupied=" << stats.occupied_hits
              << " exact=" << stats.exact_key_hits
              << " collisions=" << stats.collisions
              << " cross_search=" << stats.cross_search_hits
              << " updates=" << stats.updates
              << " informative=" << stats.informative_predictions
              << " sign_matches=" << stats.sign_matches
              << " abs_before=" << stats.absolute_error_before
              << " abs_after=" << stats.absolute_error_after
              << " saturations=" << stats.saturations << "\n";
}

inline void corrhist_profile_print() {
    print_corrhist_profile_table("old_reset_16384", corr_profile_old_reset);
    print_corrhist_profile_table("small_reset_4096", corr_profile_small_reset);
    print_corrhist_profile_table("small_persistent_4096",
                                 corr_profile_small_persistent);
}
#endif

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

inline void widen_aspiration_window(int score, int& alpha, int& beta,
                                    int& delta, bool fail_low,
                                    bool record = true) {
    if (fail_low) {
        beta = (alpha + beta) / 2;
        alpha = std::max(score - delta, -INFINITY_SCORE);
        if (record)
            Search::thread_stats[active_thread_id].aspiration_fail_lows
                .fetch_add(1, std::memory_order_relaxed);
    } else {
        beta = std::min(score + delta, INFINITY_SCORE);
        if (record)
            Search::thread_stats[active_thread_id].aspiration_fail_highs
                .fetch_add(1, std::memory_order_relaxed);
    }
    delta += delta / 2;
}

inline void widen_donor_window(int score, int previous, int& alpha, int& beta,
                               int& delta, bool fail_low) {
    CocoPolicy::widen(score, previous, fail_low, alpha, beta, delta);
    auto& stats = Search::thread_stats[active_thread_id];
    auto& failures = fail_low ? stats.aspiration_fail_lows : stats.aspiration_fail_highs;
    failures.fetch_add(1, std::memory_order_relaxed);
}

inline void update_history(int16_t& entry, int bonus) {
    int val = entry;
    val += bonus - val * std::abs(bonus) / 32768;
    entry = static_cast<int16_t>(std::clamp(val, -30000, 30000));
}

inline int draw_score(const Board& board) {
    return DRAW_SCORE + (board.get_side_to_move() == root_color ? -Search::Contempt : Search::Contempt);
}


// Called only at a non-root rule50 boundary. Checkmate takes precedence
// over a draw claim; a checked side with any legal evasion can still claim.
static int rule50_terminal_score(const Board& board, int ply) {
    const Color side = board.get_side_to_move();
    const U64 king = board.get_pieces(side, KING);
    if (!king) return -MATE_SCORE + ply;
    if (!board.is_square_attacked(get_lsb(king), side ^ 1)) return draw_score(board);
    MoveList evasions;
    generate_evasion_moves(board, evasions);
    const LegalityMasks masks = board.get_legality_masks();
    for (int i = 0; i < evasions.count; ++i)
        if (board.is_move_legal(evasions.moves[i], masks)) return draw_score(board);
    return -MATE_SCORE + ply;
}


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


thread_local int16_t four_ply_history[7][64][7][64]{};

int16_t* four_ply_entry(int ply, int piece, int to) {
    if (ply < 4 || ply - 4 >= MAX_PLY || piece >= 6) return nullptr;
    const auto& previous = search_stack[ply - 4];
    if (previous.piece < 0 || previous.piece >= 6 || unsigned(previous.to_sq) >= 64) return nullptr;
    return &four_ply_history[previous.piece][previous.to_sq][piece][to];
}

int trial_history_score(const Board& board, Move move, int ply) {
    const int piece = board.get_piece_at(move.from()) % 6;
    if (piece >= 6) return 0;
    const auto* entry = four_ply_entry(ply, piece, move.to());
    return entry ? *entry / 2 : 0;
}

void update_trial_history(const Board& board, Move move, int ply, int bonus) {
    const int piece = board.get_piece_at(move.from()) % 6;
    if (piece >= 6) return;
    if (auto* entry = four_ply_entry(ply, piece, move.to()))
        update_history(*entry, bonus);
}

void order_moves(const Board& board, const MoveList& move_list, Move tt_move, int ply, int* scores, U64 threats = 0, U64 capture_threats = 0) {
    const int mvv_lva_values[6] = { 100, 320, 330, 500, 900, 20000 };
    
    Color side = board.get_side_to_move();
    U64 pawn_threats = 0;
    if (threats) {
        const U64 pawns = board.get_pieces(side ^ 1, PAWN);
        pawn_threats = side == WHITE ? pawn_attacks_black(pawns) : pawn_attacks_white(pawns);
    }

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
            if (moved_piece >= 0 && moved_piece < 12 && victim >= 0 && victim < 6) {
                cap_hist = capture_history[moved_piece][move.to()][bool(capture_threats & (1ULL << move.to()))][victim];
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
            if (ply < MAX_PLY && move == killer_moves[ply][0]) {
                scores[i] = 900000;
            } else if (ply < MAX_PLY && move == killer_moves[ply][1]) {
                scores[i] = 800000;
            } else {
                scores[i] = get_quiet_history_score(board, move, ply, threats)
                          + trial_history_score(board, move, ply);
                if ((pawn_threats & (1ULL << move.from())) && !(threats & (1ULL << move.to()))) {
                    const int piece = board.get_piece_at(move.from()) % 6;
                    if (piece >= KNIGHT && piece <= QUEEN)
                        scores[i] += 1024;
                }
            }
        }
    }
}


#ifdef COCO_TESTING
int s9_test_order_score(const Board& board, Move move, Move tt_move, U64 threats) {
    MoveList single;
    single.moves[0] = move;
    single.count = 1;
    int score = 0;
    order_moves(board, single, tt_move, 0, &score, threats, threats);
    return score;
}
#endif

// Select in place with strict comparisons so equal scores follow the
// existing selection-sort order.
class MovePicker {
public:
    MovePicker(const Board& board, MoveList& moves, Move tt_move, int ply, U64 threats, U64 capture_threats)
        : moves_(moves) {
        order_moves(board, moves_, tt_move, ply, scores_, threats, capture_threats);
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

int quiescence(Board& board, int alpha, int beta, int ply);

// Extract Principal Variation (PV) from the Transposition Table
int get_pv_impl(Board& board, Move* pv_array, int max_pv_depth,
                bool include_bound_moves) {
    int pv_length = 0;
    U64 key = board.get_hash_key();
    int score;
    uint8_t depth;
    uint8_t flag;
    Move best_move;
    
    while (pv_length < max_pv_depth) {
        const bool found_entry = include_bound_moves
            ? search_tt().probe_entry(key, score, depth, flag, best_move, 0)
            : search_tt().probe(key, score, best_move, 0,
                                -INFINITY_SCORE, INFINITY_SCORE, 0);
        if (!found_entry || best_move.is_none()) {
            break;
        }
        
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
        if (pv_draw_boundary(board)) {
            break;
        }
        
        key = board.get_hash_key();
    }
    
    for (int i = pv_length - 1; i >= 0; i--) {
        board.unmake_move(pv_array[i]);
    }
    
    return pv_length;
}

int get_pv(Board& board, Move* pv_array, int max_pv_depth) {
    return get_pv_impl(board, pv_array, max_pv_depth, false);
}

#ifdef COCO_TESTING
thread_local uint64_t s2_reduced=0, s2_raises=0, s2_verified=0, s2_refuted=0;
thread_local uint64_t s2_unverified=0, s2_verify_nodes=0;
#endif

namespace {
struct MainEvalEntry {
    U64 key = 0;
    int score = 0;
    bool valid = false;
};
static_assert(sizeof(MainEvalEntry) == 16);
constexpr size_t MAIN_EVAL_CACHE_SIZE = 4096;
thread_local MainEvalEntry main_eval_cache[MAIN_EVAL_CACHE_SIZE];
thread_local bool main_eval_cache_active = false;

struct ScopedMainEvalCache {
    ScopedMainEvalCache() {
        std::fill_n(main_eval_cache, MAIN_EVAL_CACHE_SIZE, MainEvalEntry{});
        main_eval_cache_active = true;
    }
    ~ScopedMainEvalCache() { main_eval_cache_active = false; }
};

int main_search_eval(const Board& board) {
    if (!main_eval_cache_active)
        return Evaluation::evaluate(board);

    const U64 key = board.get_hash_key();
    auto& entry = main_eval_cache[key & (MAIN_EVAL_CACHE_SIZE - 1)];
    if (entry.valid && entry.key == key)
        return entry.score;

    const int score = Evaluation::evaluate(board);
    entry = {key, score, true};
    return score;
}
}

#ifdef COCO_TESTING
bool test_s8_cache_contract(Board& board, bool enabled) {
    const int raw = Evaluation::evaluate(board);
    const U64 key = board.get_hash_key();
    auto& slot = main_eval_cache[key & (MAIN_EVAL_CACHE_SIZE - 1)];
    slot = {key, raw + 1, true};
    if (!enabled || main_search_eval(board) != raw) return false;
    {
        ScopedMainEvalCache root;
        if (slot.valid || main_search_eval(board) != raw || !slot.valid) return false;
        slot = {key ^ MAIN_EVAL_CACHE_SIZE, raw + 1, true};
        if (main_search_eval(board) != raw || slot.key != key) return false;
        slot.score = raw + 1;
        if (main_search_eval(board) != raw + 1) return false;
    }
    if (main_search_eval(board) != raw) return false;
    ScopedMainEvalCache next_root;
    return !slot.valid && main_search_eval(board) == raw;
}
#endif


namespace {
struct RfpCorrectionEntry {
    int16_t value;
    uint16_t key;
};
static_assert(sizeof(RfpCorrectionEntry) == 4);
thread_local RfpCorrectionEntry rfp_correction[2][4096]{};
thread_local bool rfp_correction_active = false;
thread_local U64 rfp_correction_root = 0;

struct ScopedRfpCorrection {
    explicit ScopedRfpCorrection(U64 root) {
        std::memset(rfp_correction, 0, sizeof(rfp_correction));
        rfp_correction_root = root;
        rfp_correction_active = true;
    }
    ~ScopedRfpCorrection() { rfp_correction_active = false; }
};

int rfp_correction_for(Color side, uint16_t key) {
    const auto& entry = rfp_correction[side][key & 4095];
    return entry.key == key ? std::clamp(entry.value * 29 / 256, -24, 0) : 0;
}

void rfp_correction_learn(Color side, uint16_t key, int error, int depth) {
    auto& entry = rfp_correction[side][key & 4095];
    int value = entry.key == key ? entry.value : 0;
    const int bonus = std::clamp(error * depth / 8, -256, 256);
    value += bonus - value * std::abs(bonus) / 1024;
    entry = {static_cast<int16_t>(std::clamp(value, -1024, 1024)), key};
}

void rfp_correction_observe(const Board& board, int raw, int score, int depth, bool in_check,
                            Move best, uint8_t bound, Move excluded) {
    if (!rfp_correction_active || board.get_hash_key() == rfp_correction_root || in_check
        || !excluded.is_none() || best.is_none() || best.is_capture() || best.is_promotion()
        || std::abs(score) >= MATE_THRESHOLD)
        return;
    if ((bound == HASH_BETA && score <= raw) || (bound == HASH_ALPHA && score >= raw))
        return;
    rfp_correction_learn(board.get_side_to_move(), board.get_pawn_key(), score - raw, depth);
}

bool rfp_cut_allowed(const Board& board, int static_eval, int margin, int beta) {
    const int headroom = static_eval - margin - beta;
    if (headroom < 0)
        return false;
    if (headroom >= 24 || !rfp_correction_active || std::abs(beta) >= 1500
        || board.get_halfmove_clock() >= 60)
        return true;
    return headroom + rfp_correction_for(board.get_side_to_move(), board.get_pawn_key()) >= 0;
}
}

#ifdef COCO_TESTING
bool s7_rfp_veto_contract() {
    Board board;
    board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const Color side = board.get_side_to_move();
    const uint16_t key = board.get_pawn_key();
    if (!rfp_cut_allowed(board, 70, 70, 0) || rfp_cut_allowed(board, 69, 70, 0))
        return false;
    {
        ScopedRfpCorrection scope(board.get_hash_key());
        rfp_correction_learn(side, key, -600, 8);
        if (rfp_correction_for(side, key) != -24 || rfp_cut_allowed(board, 70, 70, 0)
            || !rfp_cut_allowed(board, 94, 70, 0) || rfp_cut_allowed(board, 69, 70, 0)
            || rfp_correction_for(side, key ^ 4096) != 0)
            return false;
        rfp_correction_learn(side, key ^ 4096, 600, 8);
        if (rfp_correction_for(side, key) != 0 || rfp_correction_for(side, key ^ 4096) != 0)
            return false;
        rfp_correction_learn(side, key, -600, 8);
    }
    if (!rfp_cut_allowed(board, 70, 70, 0))
        return false;
    ScopedRfpCorrection next(board.get_hash_key());
    return rfp_correction_for(side, key) == 0 && rfp_cut_allowed(board, 70, 70, 0);
}
#endif

int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, NodeType node_type, bool in_null_move_search, int parent_eval_1, int parent_eval_2, Move excluded_move, int double_ext) {
    const bool is_pv = node_type == NodeType::PV;
    reset_explicit_pv(ply);
    check_time(ply == 0);
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
    
    // Draw detection (Fifty-move rule and repetition check)
    if (ply > 0) {
        if (board.get_halfmove_clock() >= 100) return rule50_terminal_score(board, ply);
        if (board.is_repetition()) return draw_score(board);
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
            int tb_score = DRAW_SCORE;
            if (wdl == TB_WIN) {
                tb_score = VALUE_TB - ply;
            } else if (wdl == TB_CURSED_WIN) {
                tb_score = 1;
            } else if (wdl == TB_DRAW) {
                tb_score = DRAW_SCORE;
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
    
    int tt_score = 0;
    uint8_t tt_depth = 0;
    uint8_t tt_flag = 0;
    Move tt_move;
    const bool restricted_root = ply == 0
        && (root_excluded_count > 0 || root_moves_restricted);
    const auto probe_tt = [&] {
        return search_tt().probe_entry(board.get_hash_key(), tt_score, tt_depth, tt_flag, tt_move, ply);
    };
    const auto tt_can_cut = [&] {
        return tt_move != excluded_move && !restricted_root && tt_depth >= depth
            && (tt_flag == HASH_EXACT
                || (tt_flag == HASH_ALPHA && tt_score <= alpha)
                || (tt_flag == HASH_BETA && tt_score >= beta));
    };

    // Early TT cutoffs follow draw and tablebase checks and exclude special searches.
    constexpr int decisive_score = VALUE_TB - MAX_PLY;
    const bool probe_early = ply > 0 && !is_pv && !in_null_move_search
        && excluded_move.is_none() && Search::mate_limit == 0
        && board.get_halfmove_clock() + depth < 96
        && std::abs(alpha) < decisive_score && std::abs(beta) < decisive_score;
    bool tt_hit = probe_early && probe_tt();
    if (tt_hit && std::abs(tt_score) < decisive_score && tt_can_cut()) return tt_score;

    Color us = board.get_side_to_move();
    Move quiet_moves_searched[64];
    int quiet_count = 0;
    Move capture_moves_searched[64];
    int capture_count = 0;
    int king_sq = get_lsb(board.get_pieces(us, KING));
    bool in_check = board.is_square_attacked(king_sq, us ^ 1);

    // NMP and the later forward-pruning stages share one static evaluation.
    const int raw_static_eval = main_search_eval(board);
    int static_eval = raw_static_eval;
    if (ply < MAX_PLY + 4) search_stack[ply].static_eval = static_eval;
#ifdef COCO_CORRHIST_PROFILE
    if (ply < MAX_PLY + 4)
        corrhist_profile_lookup(board, search_stack[ply]);
#endif

    // Carry usable evaluations through check nodes for later improving tests.
    bool improving = false;
    if (!in_check && parent_eval_2 != INFINITY_SCORE) {
        improving = static_eval > parent_eval_2;
    }

    int next_parent_eval_1 = in_check ? parent_eval_1 : static_eval;
    int next_parent_eval_2 = in_check ? parent_eval_2 : parent_eval_1;

    bool null_searched = false;
    // Null Move Pruning (NMP)
    if (Search::mate_limit == 0 && depth >= 3 && !is_pv && !in_check && !in_null_move_search
        && excluded_move.is_none() && static_eval >= beta
        && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        const int minor_count = count_bits(board.get_pieces(us, KNIGHT)
                                          | board.get_pieces(us, BISHOP));
        const bool has_heavy = board.get_pieces(us, ROOK) || board.get_pieces(us, QUEEN);
        // Guard against zugzwang in pawn-only and lone-minor endings.
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
                null_searched = true;
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
    
    // A null search can update the table. Refresh the entry in that case.
    if (!probe_early || null_searched) {
        tt_score = 0;
        tt_depth = 0;
        tt_flag = 0;
        tt_move = Move();
        tt_hit = probe_tt();
    }
    if (tt_hit && tt_can_cut()) {
        if (ply == 0) root_search_best_move = tt_move;
        return tt_score;
    }
    if (tt_move == excluded_move
        || (ply == 0 && (!root_move_is_allowed(tt_move) || root_move_is_excluded(tt_move)))) {
        tt_move = Move();
    }
    
    // Internal Iterative Reductions (IIR)
    if (Search::mate_limit == 0 && is_pv && depth >= 3 && tt_move.is_none()) {
        depth--;
    }

    // Reverse futility and razoring both use the depth after IIR.
    if (Search::mate_limit == 0 && depth <= 3 && !is_pv && !in_check && excluded_move.is_none()
        && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        int margin = Search::RFP_Margin * depth - (improving ? 35 : 0);
        if (rfp_cut_allowed(board, static_eval, margin, beta)) {
            return static_eval;
        }
    }

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

    // ProbCut screens captures with SEE and qsearch before the reduced search.
    const CocoPolicy::ProbCut probcut = CocoPolicy::probcut(improving);
    if (Search::mate_limit == 0 && depth >= probcut.depth && !is_pv && !in_check
        && !(probcut.use_tt_veto && tt_hit && tt_score < beta + probcut.margin)
        && excluded_move.is_none() && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        const int probcut_beta = std::min(beta + probcut.margin,
                                          MATE_SCORE - MAX_PLY - 1);
        const int see_threshold = std::max(0, probcut_beta - static_eval);
        MoveList probcut_moves;
        generate_capture_moves(board, probcut_moves);
        LegalityMasks probcut_masks = board.get_legality_masks();
        MovePicker probcut_picker(board, probcut_moves, tt_move, ply, 0, probcut_masks.threats);
        Move probcut_move;

        while (probcut_picker.next(probcut_move)) {
            if (probcut_move == excluded_move || (probcut.skip_tt_move && probcut_move == tt_move)
                || board.see(probcut_move) < see_threshold
                || !board.is_move_legal(probcut_move, probcut_masks)) {
                continue;
            }

#ifdef COCO_TESTING
            if (ply == 0) ++probcut_test_attempts;
            const uint64_t probcut_nodes_before = nodes_visited;
#endif
            if (ply < MAX_PLY) {
                search_stack[ply].piece = -1;
                search_stack[ply].to_sq = -1;
                search_stack[ply].current_move = probcut_move;
            }

            board.make_move(probcut_move, true);
            search_tt().prefetch(board.get_hash_key());
            int probcut_score = -quiescence(board, -probcut_beta,
                                            -probcut_beta + 1, ply + 1);
            if (probcut_score >= probcut_beta && depth > probcut.reduction) {
                probcut_score = -alpha_beta(
                    board, -probcut_beta, -probcut_beta + 1,
                    std::max(0, depth - probcut.reduction), ply + 1, NodeType::NON_PV,
                    in_null_move_search, next_parent_eval_1, next_parent_eval_2,
                    Move(), double_ext);
            }
            board.unmake_move(probcut_move);

#ifdef COCO_TESTING
            if (ply == 0) {
                probcut_test_nodes += nodes_visited - probcut_nodes_before;
                probcut_test_last_score = probcut_score;
            }
#endif
            if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
            if (probcut_score >= probcut_beta) {
#ifdef COCO_TESTING
                if (ply == 0) ++probcut_test_cutoffs;
#endif
                search_tt().store(board.get_hash_key(), probcut_move,
                                  probcut_score, std::max(0, depth - probcut.reduction + 1),
                                  HASH_BETA, ply);
                return probcut_score;
            }
        }
    }

    // Internal Iterative Deepening (IID)
    if (tt_move.is_none() && depth >= 4 && !in_check && is_pv) {
        int iid_depth = depth - 2;
        alpha_beta(board, alpha, beta, iid_depth, ply, node_type, in_null_move_search, parent_eval_1, parent_eval_2, excluded_move, double_ext);
        // A canceled IID search may leave a partial TT move.
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
    MovePicker move_picker(board, move_list, tt_move, ply, masks.threats, masks.threats);
    
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
        if (CocoPolicy::profile != CocoPolicy::Profile::Pawnstar
            && Search::mate_limit == 0 && !move.is_capture() && !move.is_promotion()) {
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
        // Preserve moves that can change the mate distance.
        if (Search::mate_limit == 0 && moves_searched > 0 && depth <= 2 && !in_check
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
            && depth >= CocoPolicy::singular_min_depth(is_pv)
            && move == tt_move
            && excluded_move.is_none()
            && tt_hit
            && tt_depth >= depth - 3
            && tt_flag != HASH_ALPHA
            && std::abs(tt_score) < MATE_THRESHOLD) {
            
            int singular_margin = CocoPolicy::singular_margin(depth, is_pv);
            int singular_beta = tt_score - singular_margin;
            int singular_depth = CocoPolicy::singular_depth(depth);
            
            int singular_score = alpha_beta(board, singular_beta - 1, singular_beta, singular_depth, ply, NodeType::NON_PV, in_null_move_search, parent_eval_1, parent_eval_2, tt_move, double_ext);
            
            if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
            if (singular_score < singular_beta) {
                extension = CocoPolicy::singular_extension(singular_score, singular_beta, depth, is_pv, double_ext);
            } else if (CocoPolicy::profile != CocoPolicy::Profile::Maelstrom
                       && singular_score >= beta && std::abs(singular_score) < MATE_THRESHOLD) {
                return CocoPolicy::multicut_score(singular_score, beta, singular_beta);
            }
        }

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
        if (ply == 0 && moves_searched == 0) root_first_search_move = move;

        const bool history_reduction = CocoPolicy::profile == CocoPolicy::Profile::Calvin
            && depth >= 2 && !move.is_promotion() && moves_searched >= (is_pv ? 4 : 3);
        const bool pawnstar_reduction = CocoPolicy::profile == CocoPolicy::Profile::Pawnstar
            && !is_pv && depth > 2 && moves_searched > 3;
        const int quiet_history_score = history_reduction && !move.is_capture()
            ? get_quiet_history_score(board, move, ply, masks.threats) : 0;
        board.make_move(move, true);
        search_tt().prefetch(board.get_hash_key());
        legal_moves_count++;
        
        Color them = board.get_side_to_move();
        int them_king_sq = get_lsb(board.get_pieces(them, KING));
        bool gives_check = board.is_square_attacked(them_king_sq, them ^ 1);
        if (CocoPolicy::profile == CocoPolicy::Profile::Pawnstar && Search::mate_limit == 0 &&
            !is_pv && !in_check && !gives_check && depth <= 8 && moves_searched > 0 &&
            !move.is_capture() && !move.is_promotion() && legal_moves_count - 1 >= 3 + depth * depth) {
            board.unmake_move(move);
            continue;
        }

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
            if (!in_check && !gives_check && move != tt_move && pawnstar_reduction) {
                reduction = 1 + (depth > 3 && moves_searched > 6 ? 1 + (moves_searched > 12) : 0);
            } else if (!in_check && !gives_check && move != tt_move && history_reduction) {
                reduction = static_cast<int>(0.90 + std::log(depth) * std::log(moves_searched + 1) / 3.10);
                reduction += !is_pv;
                reduction -= quiet_history_score / 8192;
                // Give a late, poorly ranked quiet less initial depth only
                // when the static score already supports a cutoff.
                if (ply > 0 && ply < MAX_PLY && !is_pv && depth >= 5
                    && moves_searched >= 6 && quiet_history_score <= -256
                    && parent_eval_2 != INFINITY_SCORE && !improving && static_eval >= beta + 64
                    && !move.is_capture() && !move.is_castling()
                    && !(masks.threats & ((1ULL << move.from()) | (1ULL << move.to())))
                    && move != killer_moves[ply][0] && move != killer_moves[ply][1]
                    && !in_null_move_search && excluded_move.is_none() && Search::mate_limit == 0
                    && std::abs(alpha) < VALUE_TB - MAX_PLY && std::abs(beta) < VALUE_TB - MAX_PLY
                    && board.get_halfmove_clock() + new_depth < 96
                    && reduction > 0 && reduction + 1 < new_depth) {
                    ++reduction;
                }
            } else if (CocoPolicy::profile != CocoPolicy::Profile::Pawnstar && CocoPolicy::profile != CocoPolicy::Profile::Calvin
                && depth >= 3 && !move.is_capture() && !move.is_promotion() && !in_check &&
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
                
            }
            reduction = std::clamp(reduction, 0, std::max(0, new_depth - 1));

#ifdef COCO_TESTING
            bool s2_was_verified = false;
#endif
            if (reduction > 0) {
                score = -alpha_beta(board, -alpha - 1, -alpha,
                                    new_depth - reduction, ply + 1,
                                    NodeType::NON_PV, in_null_move_search,
                                    next_parent_eval_1, next_parent_eval_2,
                                    Move(), next_double_ext);
#ifdef COCO_TESTING
                ++s2_reduced;
                if (score > alpha && !Search::b_abort.load(std::memory_order_relaxed)) ++s2_raises;
#endif
                // A promising quiet move can fail low after losing several plies.
                // Reuse the child's cached static score to justify one extra ply.
                if (score <= alpha && score >= alpha - 32 && reduction >= 2 && !move.is_capture() && !move.is_promotion()
                    && main_eval_cache_active && !in_null_move_search && excluded_move.is_none()
                    && Search::mate_limit == 0 && std::abs(alpha) < MATE_THRESHOLD
                    && !Search::b_abort.load(std::memory_order_relaxed)) {
                    const U64 key = board.get_hash_key();
                    const auto& eval = main_eval_cache[key & (MAIN_EVAL_CACHE_SIZE - 1)];
                    if (eval.valid && eval.key == key && -eval.score >= alpha
                        && eval.score + raw_static_eval <= -32) {
                        score = -alpha_beta(board, -alpha - 1, -alpha,
                                            new_depth - reduction + 1, ply + 1,
                                            NodeType::NON_PV, in_null_move_search,
                                            next_parent_eval_1, next_parent_eval_2,
                                            Move(), next_double_ext);
                    }
                }
                // Verify a reduced alpha raise at the intended depth before
                // trusting its cutoff. Keep this null-window confirmation
                // separate from the existing full-window PV expansion.
                if (score > alpha && !Search::b_abort.load(std::memory_order_relaxed)) {
#ifdef COCO_TESTING
                    ++s2_verified;
                    s2_was_verified = true;
                    const uint64_t s2_nodes_before = nodes_visited;
#endif
                    score = -alpha_beta(board, -alpha - 1, -alpha,
                                        new_depth, ply + 1, NodeType::NON_PV,
                                        in_null_move_search, next_parent_eval_1,
                                        next_parent_eval_2, Move(), next_double_ext);
#ifdef COCO_TESTING
                    s2_verify_nodes += nodes_visited - s2_nodes_before;
                    if (!Search::b_abort.load(std::memory_order_relaxed) && score <= alpha)
                        ++s2_refuted;
#endif
                }

            } else {
                score = -alpha_beta(board, -alpha - 1, -alpha,
                                    new_depth, ply + 1, NodeType::NON_PV,
                                    in_null_move_search, next_parent_eval_1,
                                    next_parent_eval_2, Move(), next_double_ext);
            }
            
#ifdef COCO_TESTING
            if (reduction > 0 && score >= beta && !s2_was_verified
                && !Search::b_abort.load(std::memory_order_relaxed)) ++s2_unverified;
#endif
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
        
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
        
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
            if (is_pv) update_explicit_pv(ply, move);
        }
        
        if (score > alpha) {
            alpha = score;
            if (alpha >= beta) {
                if (excluded_move.is_none()) {
                    if (!restricted_root)
                        search_tt().store(board.get_hash_key(), move, score, depth, HASH_BETA, ply);
                }
                
                if (excluded_move.is_none() && ply < MAX_PLY) {
                    if (!move.is_capture() && !move.is_promotion()) {
                        if (killer_moves[ply][0] != move) {
                            killer_moves[ply][1] = killer_moves[ply][0];
                            killer_moves[ply][0] = move;
                        }
                        
                        Color side = board.get_side_to_move();
                        Piece piece_raw = board.get_piece_at(move.from());
                        PieceType piece = (PieceType)(piece_raw % 6);
                        int to = move.to();
                        
                        if (piece < 7) {
                            bool tf = masks.threats & (1ULL << move.from());
                            bool tt = masks.threats & (1ULL << move.to());
                            // quiet_count excludes the current cutoff move.
                            if (depth > 3 || quiet_count > 0) {
                                update_history(history_table[side][tf][tt][piece][to], depth * depth);
                                update_trial_history(board, move, ply, depth * depth);
                            
                                if (ply >= 1 && ply - 1 < MAX_PLY) {
                                    const SearchStack& ni1 = search_stack[ply - 1];
                                    if (ni1.piece >= 0 && ni1.piece < 7) {
                                        update_history(cont_history[0][ni1.piece][ni1.to_sq][piece][to], depth * depth);
                                    }
                                }

                                if (ply >= 2 && ply - 2 < MAX_PLY) {
                                    const SearchStack& ni2 = search_stack[ply - 2];
                                    if (ni2.piece >= 0 && ni2.piece < 7) {
                                        update_history(cont_history[1][ni2.piece][ni2.to_sq][piece][to], depth * depth);
                                    }
                                }
                            
                            }

                            for (int i = 0; i < quiet_count; i++) {
                                Move failed_move = quiet_moves_searched[i];
                                Piece failed_piece_raw = board.get_piece_at(failed_move.from());
                                PieceType failed_piece = (PieceType)(failed_piece_raw % 6);
                                int failed_to_sq = failed_move.to();
                                if (failed_piece < 7) {
                                    bool ftf = masks.threats & (1ULL << failed_move.from());
                                    bool ftt = masks.threats & (1ULL << failed_move.to());
                                    update_history(history_table[side][ftf][ftt][failed_piece][failed_to_sq], -(depth * depth));
                                    update_trial_history(board, failed_move, ply, -(depth * depth));
                                    
                                    if (ply >= 1 && ply - 1 < MAX_PLY) {
                                        const SearchStack& ni1 = search_stack[ply - 1];
                                        if (ni1.piece >= 0 && ni1.piece < 7) {
                                            update_history(cont_history[0][ni1.piece][ni1.to_sq][failed_piece][failed_to_sq], -(depth * depth));
                                        }
                                    }

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
                        Piece piece_raw = board.get_piece_at(move.from());
                        int moved_piece = piece_raw;
                        int to = move.to();
                        int victim = PAWN;
                        if (!move.is_en_passant()) {
                            victim = board.get_piece_at(move.to()) % 6;
                        }
                        
                        if (moved_piece >= 0 && moved_piece < 12 && victim >= 0 && victim < 6) {
                            update_history(capture_history[moved_piece][to][bool(masks.threats & (1ULL << to))][victim], depth * depth);
                            
                            for (int i = 0; i < capture_count; i++) {
                                Move failed_move = capture_moves_searched[i];
                                Piece failed_piece_raw = board.get_piece_at(failed_move.from());
                                int failed_moved_piece = failed_piece_raw;
                                int failed_to_sq = failed_move.to();
                                int failed_victim = PAWN;
                                if (!failed_move.is_en_passant()) {
                                    failed_victim = board.get_piece_at(failed_to_sq) % 6;
                                }
                                if (failed_moved_piece >= 0 && failed_moved_piece < 12 && failed_victim >= 0 && failed_victim < 6) {
                                    update_history(capture_history[failed_moved_piece][failed_to_sq][bool(masks.threats & (1ULL << failed_to_sq))][failed_victim], -(depth * depth));
                                }
                            }
                        }
                    }
                }
                
                if (ply == 0) root_search_best_move = move;
                rfp_correction_observe(board, raw_static_eval, score, depth, in_check,
                                       move, HASH_BETA, excluded_move);
#ifdef COCO_CORRHIST_PROFILE
                if (ply < MAX_PLY + 4)
                    corrhist_profile_update(board, search_stack[ply],
                                            raw_static_eval, score, depth,
                                            in_check, move, HASH_BETA,
                                            excluded_move);
#endif
                return score;
            }
        }
    }
    
    // Terminal position detection
    if (legal_moves_count == 0) {
        // An exclusion search with no alternative fails low at its bound.
        if (!excluded_move.is_none())
            return alpha_orig;

        if (in_check) {
            // Checkmate: score depends on distance from root (prefers shorter mates)
            return -MATE_SCORE + ply;
        } else {
            return draw_score(board);
        }
    }
    
    uint8_t flag = HASH_EXACT;
    if (best_score <= alpha_orig) {
        flag = HASH_ALPHA;
    }
#ifdef COCO_CORRHIST_PROFILE
    if (ply < MAX_PLY + 4)
        corrhist_profile_update(board, search_stack[ply], raw_static_eval,
                                best_score, depth, in_check,
                                best_move_in_node, flag, excluded_move);
#endif
    rfp_correction_observe(board, raw_static_eval, best_score, depth, in_check,
                           best_move_in_node, flag, excluded_move);
    if (ply == 0) root_search_best_move = best_move_in_node;
    if (excluded_move.is_none() && !restricted_root) {
        search_tt().store(board.get_hash_key(), best_move_in_node, best_score, depth, flag, ply);
    }
    
    return best_score;
}

int quiescence(Board& board, int alpha, int beta, int ply) {
    const bool is_pv = beta - alpha > 1;
    reset_explicit_pv(ply);
    check_time();
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;

    if (ply > 0) {
        if (board.get_halfmove_clock() >= 100) return rule50_terminal_score(board, ply);
        if (board.is_repetition()) return draw_score(board);
    }

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
    LegalityMasks masks = board.get_legality_masks();
    // Preserve the existing zero quiet-history mask for check evasions.
    MovePicker move_picker(board, move_list, tt_move, ply, 0, masks.threats);
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
            if (is_pv) update_explicit_pv(ply, move);
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
    static bool uses_game_clock = false;
    static CocoPolicy::GameClock game_clock;

    void reset_game_clock() {
        game_clock = {};
    }
    std::atomic<bool> b_abort{false};
    std::atomic<bool> pondering{false};
    std::atomic<int> active_helpers{0};
    uint64_t time_check_mask = 1023; // Default check every 1024 nodes
    uint64_t node_limit = 0;
    int mate_limit = 0;

    int num_threads = 1;
    ThreadStats thread_stats[MAX_THREADS];

    void publish_completed_result(int thread_id, Move move, int score, int depth) {
        if (thread_id < 0 || thread_id >= num_threads || move.is_none() || depth <= 0)
            return;
        ThreadStats& stats = thread_stats[thread_id];
        stats.completed_score.store(score, std::memory_order_relaxed);
        stats.completed_depth.store(depth, std::memory_order_relaxed);
        stats.completed_move.store(move.value, std::memory_order_release);
    }

    Move select_smp_voted_move(Move main_move) {
        if (num_threads <= 1) return main_move;

        int minimum_score = INFINITY_SCORE;
        for (int thread = 0; thread < num_threads; ++thread) {
            const uint16_t raw = thread_stats[thread].completed_move.load(
                std::memory_order_acquire);
            const int depth = thread_stats[thread].completed_depth.load(
                std::memory_order_relaxed);
            if (raw != 0 && depth > 0)
                minimum_score = std::min(minimum_score,
                    thread_stats[thread].completed_score.load(
                        std::memory_order_relaxed));
        }
        if (minimum_score == INFINITY_SCORE) return main_move;

        Move selected = main_move;
        uint64_t selected_weight = 0;
        for (int candidate_thread = 0; candidate_thread < num_threads;
             ++candidate_thread) {
            const uint16_t candidate_raw =
                thread_stats[candidate_thread].completed_move.load(
                    std::memory_order_acquire);
            if (candidate_raw == 0) continue;

            uint64_t total_weight = 0;
            for (int voter = 0; voter < num_threads; ++voter) {
                if (thread_stats[voter].completed_move.load(
                        std::memory_order_acquire) != candidate_raw)
                    continue;
                const int depth = std::max(0,
                    thread_stats[voter].completed_depth.load(
                        std::memory_order_relaxed));
                const int score = thread_stats[voter].completed_score.load(
                    std::memory_order_relaxed);
                const uint64_t score_bonus = static_cast<uint64_t>(
                    std::clamp(score - minimum_score, 0, 60000));
                total_weight += static_cast<uint64_t>(depth) * depth
                              + score_bonus * static_cast<uint64_t>(depth + 1);
            }

            const Move candidate(candidate_raw);
            if (total_weight > selected_weight
                || (total_weight == selected_weight && candidate == main_move)) {
                selected = candidate;
                selected_weight = total_weight;
            }
        }
        return selected.is_none() ? main_move : selected;
    }

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
        // An increment is not clock time until the move is completed.  It may
        // enlarge the preferred (soft) budget, but neither boundary may spend
        // more than the currently available clock.
        if (time_left <= 0) {
            soft_limit = 1;
            hard_limit = 1;
            return;
        }

        const uint64_t available = static_cast<uint64_t>(time_left);
        const uint64_t configured_overhead = static_cast<uint64_t>(
            std::max(0, Move_Overhead));
        const uint64_t reserve = std::min(configured_overhead, available / 2);
        const uint64_t usable = std::max<uint64_t>(1, available - reserve);
        const int horizon = moves_to_go > 0
            ? std::clamp(moves_to_go, 1, 50) : 40;
        const uint64_t base = std::max<uint64_t>(1, usable / horizon);
        const uint64_t increment_share =
            static_cast<uint64_t>(std::max(0, increment)) * 3 / 4;
        const uint64_t requested_soft = std::min(usable, base + increment_share);

        const uint64_t hard_cap = std::max<uint64_t>(1, usable * 40 / 100);
        const uint64_t minimum_hard = std::min<uint64_t>(25, hard_cap);
        const uint64_t requested_hard = std::max(
            minimum_hard, requested_soft * 5 / 2);
        hard_limit = std::min(hard_cap, requested_hard);
        soft_limit = std::min(requested_soft, hard_limit);
    }

    void compute_time_controls(Color side, const Limits& limits, int game_ply) {
        soft_limit = 0;
        hard_limit = 0;
        uses_game_clock = false;
        target_time = 0;
        node_limit = limits.nodes;
        mate_limit = limits.mate;

        if (limits.infinite) {
            // Infinite analysis is still allowed to have an explicit depth or
            // node cap, but it never consumes the supplied clock values.
        } else if (limits.movetime > 0) {
            const int requested_movetime = std::min(limits.movetime, 55000);
            int usable_movetime = requested_movetime - std::max(0, Move_Overhead);
            if (usable_movetime < 1)
                usable_movetime = std::max(1, requested_movetime / 2);
            hard_limit = static_cast<uint64_t>(
                std::min(usable_movetime, requested_movetime));
            soft_limit = hard_limit;
            target_time = start_time.load(std::memory_order_relaxed) + hard_limit;
        } else {
            int my_time = (side == WHITE) ? limits.wtime : limits.btime;
            int my_inc = (side == WHITE) ? limits.winc : limits.binc;

            if (my_time > 0) {
                const auto allocation = CocoPolicy::allocate_clock(my_time, my_inc, limits.movestogo,
                    game_ply, std::max(0, Move_Overhead), Ponder, game_clock);
                soft_limit = static_cast<uint64_t>(allocation.soft);
                hard_limit = static_cast<uint64_t>(allocation.hard);
                uses_game_clock = true;
                target_time = start_time.load(std::memory_order_relaxed) + hard_limit;
            } else if (my_time == 0) {
                allocate_time(my_time, my_inc, limits.movestogo);
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
            if (hard_limit <= 50) {
                time_check_mask = 15;
            } else if (hard_limit <= 250) {
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
                root_node_counts[t][root_node_stat_count].store(
                    0, std::memory_order_relaxed);
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
        std::memset(four_ply_history, 0, sizeof(four_ply_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i)
            search_stack[i] = SearchStack{};
        thread_stats[active_thread_id].nodes.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].tbhits.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].seldepth.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].aspiration_fail_lows.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].aspiration_fail_highs.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].completed_move.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].completed_score.store(0, std::memory_order_relaxed);
        thread_stats[active_thread_id].completed_depth.store(0, std::memory_order_relaxed);
    }

    void finish_helpers() {
        b_abort.store(true, std::memory_order_release);
        while (active_helpers.load(std::memory_order_acquire) > 0)
            std::this_thread::yield();
    }

    void search_position(Board& board, int max_depth) {
        RavagerEval::SearchScope ravager_scope(board);
        ScopedMainEvalCache main_eval_scope;
        ScopedRfpCorrection rfp_scope(board.get_hash_key());
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
#ifdef COCO_CORRHIST_PROFILE
        corrhist_profile_begin_search();
#endif

        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(four_ply_history, 0, sizeof(four_ply_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i) {
            search_stack[i] = SearchStack{};
        }
        int num_legal_moves = 0;
        Move fallback_move;
        MoveList root_list;
        generate_pseudo_legal_moves(board, root_list);
        for (int i = 0; i < root_list.count; i++) {
            if (root_move_is_allowed(root_list.moves[i]) && board.make_move(root_list.moves[i])) {
                if (num_legal_moves == 0) fallback_move = root_list.moves[i];
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
                const LegalityMasks masks = board.get_legality_masks();
                auto allowed_tablebase_move = [&](Move move) {
                    return root_move_is_allowed(move)
                        && std::find(root_list.moves, root_list.moves + root_list.count, move)
                               != root_list.moves + root_list.count
                        && board.is_move_legal(move, masks);
                };
                Move tb_move = fathom_to_coco_move(board, res);
                if (root_moves_restricted && !allowed_tablebase_move(tb_move)) {
                    res = TB_RESULT_FAILED;
                    for (const unsigned alternative : results) {
                        if (alternative == TB_RESULT_FAILED) break;
                        const Move move = fathom_to_coco_move(board, alternative);
                        if (!allowed_tablebase_move(move)) continue;

                        const auto wdl = TB_GET_WDL(alternative);
                        const auto dtz = TB_GET_DTZ(alternative);
                        if (res == TB_RESULT_FAILED || wdl > TB_GET_WDL(res)
                            || (wdl == TB_GET_WDL(res)
                                && ((wdl > TB_DRAW && dtz < TB_GET_DTZ(res))
                                    || (wdl < TB_DRAW && dtz > TB_GET_DTZ(res))))) {
                            res = alternative;
                            tb_move = move;
                        }
                    }
                }

                if (res != TB_RESULT_FAILED && allowed_tablebase_move(tb_move)) {
                    thread_stats[0].tbhits.fetch_add(1, std::memory_order_relaxed);
                    unsigned wdl = TB_GET_WDL(res);
                    unsigned dtz = TB_GET_DTZ(res);
                    int tb_score = DRAW_SCORE;
                    if (wdl == TB_WIN) {
                        tb_score = VALUE_TB - dtz - rule50;
                    } else if (wdl == TB_CURSED_WIN) {
                        tb_score = 1;
                    } else if (wdl == TB_DRAW) {
                        tb_score = DRAW_SCORE;
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
                    Move propagated[64];
                    int propagated_length = copy_explicit_pv(propagated, 64);
                    propagated_length = complete_explicit_pv_from_tt(
                        board, propagated, propagated_length, 64,
                        std::abs(score) > MATE_THRESHOLD);
                    if (propagated_length > 0 && propagated[0] == selected) {
                        for (int i = 0; i < propagated_length; ++i)
                            line.pv.push_back(propagated[i]);
                    } else {
                        line.pv.push_back(selected);
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

            Move best_move = fallback_move;
            Move ponder_move;
            if (!completed_lines.empty()) {
                best_move = completed_lines.front().move;
                if (completed_lines.front().pv.size() > 1)
                    ponder_move = completed_lines.front().pv[1];
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
        int score_stability_count = 0;
        bool all_moves_stable = true;
        int lowest_score = INFINITY_SCORE;
        int highest_score = -INFINITY_SCORE;
        const bool pz61_clock = uses_game_clock && CocoPolicy::profile == CocoPolicy::Profile::PZ61;
        const int initial_eval = pz61_clock ? Evaluation::evaluate(board) : 0;
        const auto root_side = board.get_side_to_move();
        const bool root_in_check = pz61_clock &&
            board.is_square_attacked(get_lsb(board.get_pieces(root_side, KING)), root_side ^ 1);
        double time_reduction = 1;
        uint64_t previous_soft_limit = soft_limit;
        bool completed_iteration = false;
        
        int target_depth = max_depth;
        int last_score = 0;

        for (int current_depth = 1; current_depth <= target_depth; ++current_depth) {
            int score = 0;

            const auto aspiration = CocoPolicy::aspiration(last_score);
            if (current_depth < aspiration.depth) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, NodeType::PV, false);
            } else {
                int delta = aspiration.delta;
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
                        widen_donor_window(score, last_score, alpha, beta, delta, true);
                    } else if (score >= beta) {
                        widen_donor_window(score, last_score, alpha, beta, delta, false);
                    } else {
                        break;
                    }
                }
            }
            
            if (b_abort.load(std::memory_order_relaxed)) {
                break;
            }
            
            // Single legal move optimization: immediately break after completing depth 1
            if (num_legal_moves == 1 && current_depth >= 1) {
                break;
            }
            
            // Consume the recursively propagated PV from this completed
            // iteration. Helpers may update the shared TT concurrently, so a
            // post-search TT walk is not authoritative for the main line.
            Move pv[64];
            int pv_len = copy_explicit_pv(pv, 64);
            pv_len = complete_explicit_pv_from_tt(
                board, pv, pv_len, 64, std::abs(score) > MATE_THRESHOLD);
            if (pv_len > 0) {
                last_completed_best_move = pv[0];
            } else if (!root_search_best_move.is_none()) {
                last_completed_best_move = root_search_best_move;
            }
            last_completed_ponder_move = pv_len > 1 ? pv[1] : Move();
            publish_completed_result(0, last_completed_best_move, score, current_depth);

            Move current_best_move = last_completed_best_move;
            if (current_depth > 1) {
                if (!current_best_move.is_none() && !previous_best_move.is_none() && current_best_move == previous_best_move) {
                    best_move_stability_count++;
                } else {
                    best_move_stability_count = 0;
                    all_moves_stable = false;
                }
                score_stability_count = std::abs(score - last_score) <= 10 ? score_stability_count + 1 : 0;
            }
            previous_best_move = current_best_move;

            const int previous_score = last_score;
            const bool all_scores_stable = score - lowest_score <= 50 && highest_score - score <= 50;
            lowest_score = std::min(lowest_score, score);
            highest_score = std::max(highest_score, score);
            uint64_t adjusted_soft_limit = soft_limit;
            if (uses_game_clock) {
                const uint64_t accounted_root_nodes = root_nodes_total();
                const uint64_t best_root_nodes = current_best_move.is_none()
                    ? 0 : root_nodes_for_move(current_best_move);
                const double fraction = accounted_root_nodes
                    ? static_cast<double>(best_root_nodes) / accounted_root_nodes : 0.0;
                const bool quiet_best = !root_in_check && !current_best_move.is_none()
                    && !current_best_move.is_capture() && !current_best_move.is_promotion();
                const CocoPolicy::RootIteration root{current_depth, score, previous_score, initial_eval,
                    best_move_stability_count, score_stability_count, all_moves_stable,
                    all_scores_stable, quiet_best, current_best_move == root_first_search_move,
                    root_node_stat_count == 1, accounted_root_nodes, fraction};
                const double multiplier = CocoPolicy::soft_multiplier(root, game_clock, time_reduction);
                double target = soft_limit * multiplier;
                if (root.single_move && (CocoPolicy::profile == CocoPolicy::Profile::Stockfish18 ||
                                         CocoPolicy::profile == CocoPolicy::Profile::Stockfish19))
                    target = std::min(target, CocoPolicy::profile == CocoPolicy::Profile::Stockfish19 ? 500.0 : 502.0);
                adjusted_soft_limit = static_cast<uint64_t>(std::clamp(target, 1.0, double(hard_limit)));
                if (CocoPolicy::profile == CocoPolicy::Profile::Rice || CocoPolicy::profile == CocoPolicy::Profile::Maelstrom) {
                    std::swap(adjusted_soft_limit, previous_soft_limit);
                    adjusted_soft_limit = std::min(adjusted_soft_limit, hard_limit);
                }
            }

            last_score = score;
            completed_iteration = true;

            thread_stats[0].nodes.store(nodes_visited, std::memory_order_relaxed);
            thread_stats[0].seldepth.store(max_ply_reached, std::memory_order_relaxed);

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
        if (uses_game_clock && completed_iteration) {
            game_clock.previous_time_reduction = time_reduction;
            game_clock.previous_score = last_score;
            game_clock.have_score = true;
        }
        
        if (last_completed_best_move.is_none())
            last_completed_best_move = fallback_move;

        while (pondering.load(std::memory_order_relaxed)
               && !b_abort.load(std::memory_order_relaxed))
            std::this_thread::yield();
        finish_helpers();

        const Move voted_move = select_smp_voted_move(last_completed_best_move);
        if (voted_move != last_completed_best_move && !voted_move.is_none()) {
            const LegalityMasks masks = board.get_legality_masks();
            if (root_move_is_allowed(voted_move)
                && board.is_move_legal(voted_move, masks)) {
                last_completed_best_move = voted_move;
                last_completed_ponder_move = Move();
                if (board.make_move(voted_move, true)) {
                    Move tail[1];
                    if (!pv_draw_boundary(board) && get_pv(board, tail, 1) > 0)
                        last_completed_ponder_move = tail[0];
                    board.unmake_move(voted_move);
                }
            }
        }

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

#ifdef COCO_CORRHIST_PROFILE
        corrhist_profile_print();
#endif
        std::cout << "bestmove " << move_to_str(last_completed_best_move);
        if (Ponder && !last_completed_ponder_move.is_none())
            std::cout << " ponder " << move_to_str(last_completed_ponder_move);
        std::cout << "\n";
        std::cout << std::flush;
    }

    void search_helper(Board& board, int max_depth, int thread_id) {
        RavagerEval::SearchScope ravager_scope(board);
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = thread_id;
        root_accounting_enabled = true;
        if (b_abort.load(std::memory_order_relaxed)) {
            active_helpers.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(four_ply_history, 0, sizeof(four_ply_history));
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

            const auto aspiration = CocoPolicy::aspiration(last_score);
            if (current_depth < aspiration.depth) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, NodeType::PV, false);
            } else {
                // Vary aspiration delta by thread to avoid synchronized fail-highs/lows
                int delta = aspiration.delta + (thread_id % 4) * 4;
                int alpha = std::max(last_score - delta, -INFINITY_SCORE);
                int beta = std::min(last_score + delta, INFINITY_SCORE);

                while (true) {
                    if (b_abort.load(std::memory_order_relaxed)) break;

                    score = alpha_beta(board, alpha, beta, current_depth, 0, NodeType::PV, false);

                    if (b_abort.load(std::memory_order_relaxed)) break;

                    if (score <= alpha) {
                        widen_donor_window(score, last_score, alpha, beta, delta, true);
                    } else if (score >= beta) {
                        widen_donor_window(score, last_score, alpha, beta, delta, false);
                    } else {
                        break;
                    }
                }
            }

            thread_stats[thread_id].nodes.store(nodes_visited, std::memory_order_relaxed);
            thread_stats[thread_id].seldepth.store(max_ply_reached, std::memory_order_relaxed);

            if (b_abort.load(std::memory_order_relaxed)) break;

            last_score = score;
            publish_completed_result(thread_id, root_search_best_move,
                                     score, current_depth);
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

    ProbCutTestResult test_probcut_window(Board& board, int alpha, int beta,
                                          int depth, NodeType node_type) {
        nodes_visited = 0;
        max_ply_reached = 0;
        active_thread_id = 0;
        start_time.store(get_time_ms(), std::memory_order_relaxed);
        root_color = board.get_side_to_move();
        probcut_test_attempts = 0;
        probcut_test_cutoffs = 0;
        probcut_test_nodes = 0;
        probcut_test_last_score = -INFINITY_SCORE;
        b_abort.store(false, std::memory_order_relaxed);
        hard_limit = 0;
        node_limit = 0;
        const int score = ::alpha_beta(board, alpha, beta, depth, 0, node_type);
        return {score, probcut_test_attempts, probcut_test_cutoffs,
                probcut_test_nodes, probcut_test_last_score};
    }

    AspirationWindowTestResult test_widen_aspiration(
        int score, int alpha, int beta, int delta, bool fail_low) {
        widen_aspiration_window(score, alpha, beta, delta, fail_low, false);
        return {alpha, beta, delta};
    }

    double test_root_node_fraction_multiplier(uint64_t best_nodes,
                                              uint64_t total_nodes) {
        return root_node_fraction_multiplier(best_nodes, total_nodes);
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

    Move test_select_smp_voted_move(Move main_move) {
        return select_smp_voted_move(main_move);
    }

    int test_get_explicit_pv(Move* pv, int max_depth) {
        return copy_explicit_pv(pv, max_depth);
    }

    int test_get_completed_explicit_pv(Board& board, Move* pv, int max_depth) {
        const int length = copy_explicit_pv(pv, max_depth);
        return complete_explicit_pv_from_tt(
            board, pv, length, max_depth, true);
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
