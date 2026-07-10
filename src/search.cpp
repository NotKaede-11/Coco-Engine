#include "search.h"
#include "types.h"
#include "movegen.h"
#include "tt.h"
#include "evaluate.h"
#include <iostream>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstring>

// Constants for evaluation and search bounds
const int MATE_SCORE = 30000;
const int MATE_THRESHOLD = 29000;

// Nodes visited counter
thread_local uint64_t nodes_visited = 0;

// Maximum ply reached in search (for seldepth reporting)
thread_local int max_ply_reached = 0;

// Time utility in milliseconds
inline uint64_t get_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

// Check time and trigger abort if hard boundary is exceeded
inline void check_time() {
    if (Search::hard_limit != 0 && (nodes_visited & Search::time_check_mask) == 0) {
        uint64_t elapsed = get_time_ms() - Search::start_time;
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
thread_local int history_table[2][2][2][7][64]; // [color][threat_from][threat_to][piece_type][to_square] - size 7 to prevent out-of-bounds on NO_PIECE_TYPE
int lmr_table[64][64];

struct NodeInfo {
    int piece = -1;
    int to_sq = -1;
};
thread_local NodeInfo node_info[MAX_PLY + 4];
thread_local int16_t cont_history[2][7][64][7][64];
thread_local int16_t capture_history[12][64][6];

inline void update_history(int16_t& entry, int bonus) {
    int val = entry;
    val += bonus - val * std::abs(bonus) / 32768;
    entry = static_cast<int16_t>(std::clamp(val, -30000, 30000));
}


// Move Ordering helper function using stack memory
void order_moves(const Board& board, MoveList& move_list, Move tt_move, int ply, U64 threats = 0) {
    int scores[256] = {0};
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
            int capture_value = mvv_lva_values[victim] * 10 - assailant;
            
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
                Piece piece_raw = board.get_piece_at(move.from());
                PieceType piece = (PieceType)(piece_raw % 6);
                int to = move.to();
                
                bool tf = threats & (1ULL << move.from());
                bool tt = threats & (1ULL << move.to());
                
                // Safety bound check
                if (piece < 7) {
                    scores[i] = history_table[side][tf][tt][piece][to];
                    
                    // CMH (ply-1)
                    if (ply >= 1 && ply - 1 < MAX_PLY) {
                        const NodeInfo& ni1 = node_info[ply - 1];
                        if (ni1.piece >= 0 && ni1.piece < 7) {
                            scores[i] += cont_history[0][ni1.piece][ni1.to_sq][piece][to];
                        }
                    }
                    
                    // FMH (ply-2)
                    if (ply >= 2 && ply - 2 < MAX_PLY) {
                        const NodeInfo& ni2 = node_info[ply - 2];
                        if (ni2.piece >= 0 && ni2.piece < 7) {
                            scores[i] += cont_history[1][ni2.piece][ni2.to_sq][piece][to];
                        }
                    }
                } else {
                    scores[i] = 0;
                }
            }
        }
    }
    
    // Sort using selection sort (zero heap allocations)
    for (int i = 0; i < move_list.count - 1; i++) {
        int best_idx = i;
        for (int j = i + 1; j < move_list.count; j++) {
            if (scores[j] > scores[best_idx]) {
                best_idx = j;
            }
        }
        if (best_idx != i) {
            std::swap(move_list.moves[i], move_list.moves[best_idx]);
            std::swap(scores[i], scores[best_idx]);
        }
    }
}

// Forward declarations of search functions
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, bool is_pv, bool in_null_move_search = false, int parent_eval_1 = INFINITY_SCORE, int parent_eval_2 = INFINITY_SCORE, Move excluded_move = Move(), int double_ext = 0);
int quiescence(Board& board, int alpha, int beta, int ply);

// Extract Principal Variation (PV) from the Transposition Table
int get_pv(Board& board, Move* pv_array, int max_pv_depth) {
    int pv_length = 0;
    U64 key = board.get_hash_key();
    int score;
    Move best_move;
    
    while (pv_length < max_pv_depth) {
        // Probe TT with depth 0 to retrieve best move without score restriction
        if (!tt.probe(key, score, best_move, 0, -INFINITY_SCORE, INFINITY_SCORE, 0) || best_move.is_none()) {
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
        
        key = board.get_hash_key();
    }
    
    // Restore the board back to the original root state
    for (int i = pv_length - 1; i >= 0; i--) {
        board.unmake_move(pv_array[i]);
    }
    
    return pv_length;
}

// Alpha-Beta Search Core with Null Move Pruning (NMP)
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, bool is_pv, bool in_null_move_search, int parent_eval_1, int parent_eval_2, Move excluded_move, int double_ext) {
    // Cooperative search abortion check
    check_time();
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
    
    // Draw detection (Fifty-move rule and repetition check)
    if (ply > 0 && (board.get_halfmove_clock() >= 100 || board.is_repetition())) {
        return 0;
    }
    
    nodes_visited++;
    if (ply > max_ply_reached) {
        max_ply_reached = ply;
    }
    
    if (depth <= 0) {
        return quiescence(board, alpha, beta, ply);
    }
    
    Color us = board.get_side_to_move();
    Move quiet_moves_searched[64];
    int quiet_count = 0;
    Move capture_moves_searched[64];
    int capture_count = 0;
    int king_sq = get_lsb(board.get_pieces(us, KING));
    bool in_check = board.is_square_attacked(king_sq, us ^ 1);
    
    // Null Move Pruning (NMP)
    if (depth >= 3 && !is_pv && !in_check && !in_null_move_search && excluded_move.is_none()) {
        bool has_non_pawn_material = board.get_pieces(us, KNIGHT) || 
                                     board.get_pieces(us, BISHOP) || 
                                     board.get_pieces(us, ROOK) || 
                                     board.get_pieces(us, QUEEN);
        if (has_non_pawn_material) {
            int R = Search::NMP_Base + (depth / Search::NMP_Divisor);
            if (ply < MAX_PLY) {
                node_info[ply].piece = -1;
                node_info[ply].to_sq = -1;
            }
            board.make_null_move();
            int null_score = -alpha_beta(board, -beta, -beta + 1, depth - 1 - R, ply + 1, false, true, parent_eval_1, parent_eval_2, excluded_move, double_ext);
            board.unmake_null_move();
            
            if (null_score >= beta) {
                return beta;
            }
        }
    }
    
    // Probe the Transposition Table
    int tt_score = 0;
    uint8_t tt_depth = 0;
    uint8_t tt_flag = 0;
    Move tt_move;
    bool tt_hit = tt.probe_entry(board.get_hash_key(), tt_score, tt_depth, tt_flag, tt_move, ply);
    
    if (tt_hit && tt_move != excluded_move) {
        if (tt_depth >= depth) {
            if (tt_flag == HASH_EXACT) return tt_score;
            if (tt_flag == HASH_ALPHA && tt_score <= alpha) return alpha;
            if (tt_flag == HASH_BETA && tt_score >= beta) return beta;
        }
    }
    if (tt_move == excluded_move) {
        tt_move = Move();
    }
    
    // Internal Iterative Reductions (IIR)
    if (is_pv && depth >= 3 && tt_move.is_none()) {
        depth--;
    }

    // Retrieve static evaluation
    int static_eval = Evaluation::evaluate(board);
    
    // Compute improving: static evaluation is better than 2 plies ago
    bool improving = false;
    if (!in_check && parent_eval_2 != INFINITY_SCORE) {
        improving = static_eval > parent_eval_2;
    }
    
    int next_parent_eval_1 = in_check ? parent_eval_1 : static_eval;
    int next_parent_eval_2 = in_check ? parent_eval_2 : parent_eval_1;
    
    // Reverse Futility Pruning (RFP) / Static Null Move Pruning
    // Strictly disabled if beta is near a checkmate score to prevent mate-blindness tactical leaks
    if (depth <= 3 && !is_pv && !in_check && excluded_move.is_none() && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        int margin = Search::RFP_Margin * depth - (improving ? 35 : 0);
        if (static_eval - margin >= beta) {
            return static_eval;
        }
    }
    
    // Razoring
    if (depth == 1 && !is_pv && !in_check && excluded_move.is_none() && alpha > -INFINITY_SCORE + 1000) {
        int razor_margin = 300;
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
        alpha_beta(board, alpha, beta, iid_depth, ply, is_pv, in_null_move_search, parent_eval_1, parent_eval_2, excluded_move, double_ext);
        // Time-abort check: immediately return 0 and skip probing the TT to prevent move pollution
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
        int dummy_score;
        tt.probe(board.get_hash_key(), dummy_score, tt_move, 0, -INFINITY_SCORE, INFINITY_SCORE, ply);
    }
    
    int alpha_orig = alpha;
    
    LegalityMasks masks = board.get_legality_masks();
    
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    order_moves(board, move_list, tt_move, ply, masks.threats);
    
    int legal_moves_count = 0;
    int best_score = -INFINITY_SCORE;
    Move best_move_in_node;
    int moves_searched = 0;
 
    for (int i = 0; i < move_list.count; i++) {
        Move move = move_list.moves[i];
        
        if (move == excluded_move) {
            continue;
        }
        
        // Late Move Pruning (LMP)
        if (!move.is_capture() && !move.is_promotion()) {
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
        if (depth <= 2 && !in_check && !move.is_capture() && !move.is_promotion()) {
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
        
        // Symmetrical illegal move filtering
        if (!board.is_move_legal(move, masks)) {
            continue;
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
            
            int singular_score = alpha_beta(board, singular_beta - 1, singular_beta, singular_depth, ply, false, in_null_move_search, parent_eval_1, parent_eval_2, tt_move, double_ext);
            
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
                node_info[ply].piece = (int)p_type;
                node_info[ply].to_sq = move.to();
            } else {
                node_info[ply].piece = -1;
                node_info[ply].to_sq = -1;
            }
        }
        
        board.make_move(move, true);
        legal_moves_count++;
        
        // Check if this move gives check
        Color them = board.get_side_to_move();
        int them_king_sq = get_lsb(board.get_pieces(them, KING));
        bool gives_check = board.is_square_attacked(them_king_sq, them ^ 1);
        
        int score;
        if (moves_searched == 0) {
            score = -alpha_beta(board, -beta, -alpha, depth - 1 + extension, ply + 1, is_pv, in_null_move_search, next_parent_eval_1, next_parent_eval_2, Move(), next_double_ext);
        } else {
            bool lmr_applied = false;
            
            // Late Move Reductions (LMR) - Exclude PV nodes
            if (depth >= 3 && !move.is_capture() && !move.is_promotion() && !in_check &&
                move != tt_move && !gives_check && moves_searched >= 4 && !is_pv) {
                
                int d_idx = std::min(depth, 63);
                int m_idx = std::min(moves_searched, 63);
                int reduction = lmr_table[d_idx][m_idx];
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
                reduction = std::max(0, reduction);
                
                if (depth - 1 - reduction < 1) {
                    reduction = depth - 2; // Cap reduction so remaining depth is exactly 1
                }
                
                score = -alpha_beta(board, -alpha - 1, -alpha, depth - 1 - reduction, ply + 1, false, in_null_move_search, next_parent_eval_1, next_parent_eval_2, Move(), next_double_ext);
                lmr_applied = true;
            } else {
                score = -alpha_beta(board, -alpha - 1, -alpha, depth - 1, ply + 1, false, in_null_move_search, next_parent_eval_1, next_parent_eval_2, Move(), next_double_ext);
            }
            
            // Re-search trigger
            if (score > alpha && score < beta) {
                score = -alpha_beta(board, -beta, -alpha, depth - 1, ply + 1, is_pv, in_null_move_search, next_parent_eval_1, next_parent_eval_2, Move(), next_double_ext);
            }
        }
        
        board.unmake_move(move);
        
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
                    tt.store(board.get_hash_key(), move, beta, depth, HASH_BETA, ply);
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
                            history_table[side][tf][tt][piece][to] += depth * depth;
                            
                            // CMH (ply-1)
                            if (ply >= 1 && ply - 1 < MAX_PLY) {
                                const NodeInfo& ni1 = node_info[ply - 1];
                                if (ni1.piece >= 0 && ni1.piece < 7) {
                                    update_history(cont_history[0][ni1.piece][ni1.to_sq][piece][to], depth * depth);
                                }
                            }

                            // FMH (ply-2)
                            if (ply >= 2 && ply - 2 < MAX_PLY) {
                                const NodeInfo& ni2 = node_info[ply - 2];
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
                                    history_table[side][ftf][ftt][failed_piece][failed_to_sq] -= depth * depth;
                                    
                                    // CMH malus
                                    if (ply >= 1 && ply - 1 < MAX_PLY) {
                                        const NodeInfo& ni1 = node_info[ply - 1];
                                        if (ni1.piece >= 0 && ni1.piece < 7) {
                                            update_history(cont_history[0][ni1.piece][ni1.to_sq][failed_piece][failed_to_sq], -(depth * depth));
                                        }
                                    }

                                    // FMH malus
                                    if (ply >= 2 && ply - 2 < MAX_PLY) {
                                        const NodeInfo& ni2 = node_info[ply - 2];
                                        if (ni2.piece >= 0 && ni2.piece < 7) {
                                            update_history(cont_history[1][ni2.piece][ni2.to_sq][failed_piece][failed_to_sq], -(depth * depth));
                                        }
                                    }
                                }
                            }
                            
                            // Prevent overflow via aging mechanism checking absolute values safely
                            if (history_table[side][tf][tt][piece][to] > Search::History_Threshold || history_table[side][tf][tt][piece][to] < -Search::History_Threshold) {
                                for (int c = 0; c < 2; ++c) {
                                    for (int tf_idx = 0; tf_idx < 2; ++tf_idx) {
                                        for (int tt_idx = 0; tt_idx < 2; ++tt_idx) {
                                            for (int p = 0; p < 7; ++p) {
                                                for (int s = 0; s < 64; ++s) {
                                                    history_table[c][tf_idx][tt_idx][p][s] /= 2;
                                                }
                                            }
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
                
                return beta;
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
            return 0;
        }
    }
    
    // Save search result in Transposition Table
    uint8_t flag = HASH_EXACT;
    if (best_score <= alpha_orig) {
        flag = HASH_ALPHA;
    }
    if (excluded_move.is_none()) {
        tt.store(board.get_hash_key(), best_move_in_node, best_score, depth, flag, ply);
    }
    
    return best_score;
}

// Quiescence Search
int quiescence(Board& board, int alpha, int beta, int ply) {
    check_time();
    if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
    
    nodes_visited++;
    if (ply > max_ply_reached) {
        max_ply_reached = ply;
    }
    
    // Standing pat baseline evaluation (calling formal NNUE evaluation)
    int stand_pat = Evaluation::evaluate(board);
    if (stand_pat >= beta) {
        return beta;
    }
    if (stand_pat > alpha) {
        alpha = stand_pat;
    }
    
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    order_moves(board, move_list, Move(), ply);
    
    LegalityMasks masks = board.get_legality_masks();
    int moves_searched = 0;

    for (int i = 0; i < move_list.count; i++) {
        Move move = move_list.moves[i];
        
        // Quiescence search must only evaluate capture sequences
        if (!move.is_capture()) {
            continue;
        }

        // Safe QS Futility Pruning
        if (moves_searched >= 2 && !move.is_promotion()) {
            Piece captured = board.get_piece_at(move.to());
            int cap_type = (captured != NO_PIECE) ? (captured % 6) : PAWN;
            const int piece_values[6] = { 100, 320, 330, 500, 900, 20000 };
            if (stand_pat + 350 + piece_values[cap_type] <= alpha) {
                continue;
            }
        }

        // Static Exchange Evaluation: prune losing captures
        if (board.see(move) < 0) {
            continue;
        }
        
        if (!board.is_move_legal(move, masks)) {
            continue;
        }
        board.make_move(move, true);
        moves_searched++;
        
        int score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmake_move(move);
        
        if (Search::b_abort.load(std::memory_order_relaxed)) return 0;
        
        if (score >= beta) {
            return beta;
        }
        if (score > alpha) {
            alpha = score;
        }
    }
    
    return alpha;
}

namespace Search {
    uint64_t start_time = 0;
    uint64_t target_time = 0;
    uint64_t soft_limit = 0;
    uint64_t hard_limit = 0;
    std::atomic<bool> b_abort{false};
    uint64_t time_check_mask = 1023; // Default check every 1024 nodes

    int num_threads = 1;
    ThreadStats thread_stats[MAX_THREADS];

    int RFP_Margin = 74;
    int LMR_Constant_Scaled = 224;
    int NMP_Base = 2;
    int NMP_Divisor = 6;
    int Aspiration_Delta = 18;
    int History_Threshold = 16367;
    int Move_Overhead = 30;

    void init_search_tables() {
        for (int d = 1; d < 64; d++) {
            for (int m = 1; m < 64; m++) {
                lmr_table[d][m] = static_cast<int>(0.5 + log(d) * log(m) / (static_cast<double>(LMR_Constant_Scaled) / 100.0));
            }
        }
    }

    void allocate_time(int time_left, int increment) {
        int usable_time = time_left - Move_Overhead;
        if (usable_time <= 0) {
            usable_time = 10;
        }

        soft_limit = static_cast<uint64_t>((usable_time / 40) + increment);

        int calculated_multiplier = static_cast<int>(soft_limit * 2.5);
        int absolute_max_cap = static_cast<int>(usable_time * 0.40);
        hard_limit = static_cast<uint64_t>(std::min(calculated_multiplier, absolute_max_cap));

        if (soft_limit < 15) soft_limit = 15;
        if (hard_limit < 25) hard_limit = 25;
    }

    // Compute time controls from clock parameters (called before thread launch)
    void compute_time_controls(Color side, int max_depth, int wtime, int btime, int winc, int binc, int movetime) {
        if (movetime > 0) {
            int usable_movetime = movetime - Move_Overhead;
            if (usable_movetime < 10) {
                usable_movetime = std::max(5, movetime / 2);
            }
            hard_limit = static_cast<uint64_t>(std::min(usable_movetime, 55000));
            soft_limit = hard_limit;
            target_time = start_time + hard_limit;
        } else {
            int my_time = (side == WHITE) ? wtime : btime;
            int my_inc = (side == WHITE) ? winc : binc;

            if (my_time > 0) {
                allocate_time(my_time, my_inc);

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
                target_time = start_time + hard_limit;
            } else if (my_time == 0) {
                soft_limit = 10;
                hard_limit = 20;
                target_time = start_time + hard_limit;
            } else {
                soft_limit = 0;
                hard_limit = 0;
                target_time = 0;
            }
        }

        if (hard_limit > 0) {
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

    // Master search control loop with Iterative Deepening (main thread only)
    void search_position(Board& board, int max_depth) {
        // Reset killer moves and history table at start of search
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i) {
            node_info[i].piece = -1;
            node_info[i].to_sq = -1;
        }
        // Count legal moves at the root
        int num_legal_moves = 0;
        MoveList root_list;
        generate_pseudo_legal_moves(board, root_list);
        for (int i = 0; i < root_list.count; i++) {
            if (board.make_move(root_list.moves[i])) {
                num_legal_moves++;
                board.unmake_move(root_list.moves[i]);
            }
        }

        Move best_move;
        Move last_completed_best_move;
        Move previous_best_move = Move();
        int best_move_stability_count = 0;
        
        int target_depth = max_depth;
        int last_score = 0;

        // Iterative Deepening loop
        for (int current_depth = 1; current_depth <= target_depth; ++current_depth) {
            nodes_visited = 0;
            max_ply_reached = 0;

            int score = 0;

            if (current_depth < 3) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, true, false);
            } else {
                int delta = Aspiration_Delta;
                int alpha = std::max(last_score - delta, -INFINITY_SCORE);
                int beta = std::min(last_score + delta, INFINITY_SCORE);
                
                while (true) {
                    check_time();
                    if (b_abort.load(std::memory_order_relaxed)) {
                        break;
                    }

                    score = alpha_beta(board, alpha, beta, current_depth, 0, true, false);

                    if (b_abort.load(std::memory_order_relaxed)) {
                        break;
                    }
                    
                    if (score <= alpha) {
                        // Fail-low: the position is worse than anticipated. Drop alpha (expand lower bound)
                        // while keeping beta unchanged since the score is guaranteed not to exceed it.
                        alpha = std::max(alpha - delta, -INFINITY_SCORE);
                        delta += delta / 2;
                    } else if (score >= beta) {
                        // Fail-high: a tactical threat or opportunity is discovered. Widen beta (expand upper bound)
                        // while keeping alpha unchanged since the score is guaranteed not to drop below it.
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
            tt.probe(board.get_hash_key(), temp_score, best_move_depth, current_depth, -INFINITY_SCORE, INFINITY_SCORE, 0);
            
            if (!best_move_depth.is_none()) {
                last_completed_best_move = best_move_depth;
            }
            
            // Extract the PV from the TT cache
            Move pv[64];
            int pv_len = get_pv(board, pv, 64);
            if (pv_len > 0 && last_completed_best_move.is_none()) {
                last_completed_best_move = pv[0];
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
            thread_stats[0].nodes = nodes_visited;
            thread_stats[0].seldepth = max_ply_reached;

            // Aggregate metrics across all threads
            uint64_t total_nodes = 0;
            int total_seldepth = max_ply_reached;
            for (int t = 0; t < num_threads; t++) {
                total_nodes += thread_stats[t].nodes;
                total_seldepth = std::max(total_seldepth, thread_stats[t].seldepth);
            }

            uint64_t elapsed = get_time_ms() - start_time;
            uint64_t nps = elapsed > 0 ? (total_nodes * 1000) / elapsed : total_nodes * 1000;

            // Print standardized UCI info line
            std::cout << "info depth " << current_depth << " seldepth " << total_seldepth << " ";

            if (score > MATE_THRESHOLD) {
                int plies = MATE_SCORE - score;
                std::cout << "score mate " << ((plies + 1) / 2);
            } else if (score < -MATE_THRESHOLD) {
                int plies = score + MATE_SCORE;
                std::cout << "score mate -" << ((plies + 1) / 2);
            } else {
                std::cout << "score cp " << score;
            }

            std::cout << " nodes " << total_nodes << " time " << elapsed << " nps " << nps << " pv";

            for (int i = 0; i < pv_len; i++) {
                std::cout << " " << move_to_str(pv[i]);
            }
            std::cout << "\n";

            if (adjusted_soft_limit != 0 && elapsed > adjusted_soft_limit) {
                break;
            }
        }
        
        // Fallback: if search aborted before completion of depth 1, select first legal move
        if (last_completed_best_move.is_none()) {
            MoveList list;
            generate_pseudo_legal_moves(board, list);
            for (int i = 0; i < list.count; i++) {
                if (board.make_move(list.moves[i])) {
                    last_completed_best_move = list.moves[i];
                    board.unmake_move(list.moves[i]);
                    break;
                }
            }
        }
        
        // Report final best move designation to GUI
        std::cout << "bestmove " << move_to_str(last_completed_best_move) << "\n";
        std::cout << std::flush;
    }

    // Helper thread search entry point (threads 1..N-1)
    void search_helper(Board& board, int max_depth, int thread_id) {
        // Clear thread-local killer moves and history table
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        std::memset(history_table, 0, sizeof(history_table));
        std::memset(cont_history, 0, sizeof(cont_history));
        std::memset(capture_history, 0, sizeof(capture_history));
        for (int i = 0; i < MAX_PLY + 4; ++i) {
            node_info[i].piece = -1;
            node_info[i].to_sq = -1;
        }

        int target_depth = max_depth > 0 ? max_depth : MAX_PLY;
        // Depth stagger: odd thread IDs skip depth 1 for search diversity
        int start_depth = (thread_id % 2 == 1) ? 2 : 1;
        int last_score = 0;

        for (int current_depth = start_depth; current_depth <= target_depth; ++current_depth) {
            nodes_visited = 0;
            max_ply_reached = 0;

            int score = 0;

            if (current_depth < 3) {
                score = alpha_beta(board, -INFINITY_SCORE, INFINITY_SCORE, current_depth, 0, true, false);
            } else {
                int delta = Aspiration_Delta;
                int alpha = std::max(last_score - delta, -INFINITY_SCORE);
                int beta = std::min(last_score + delta, INFINITY_SCORE);

                while (true) {
                    if (b_abort.load(std::memory_order_relaxed)) break;

                    score = alpha_beta(board, alpha, beta, current_depth, 0, true, false);

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
            thread_stats[thread_id].nodes = nodes_visited;
            thread_stats[thread_id].seldepth = max_ply_reached;

            if (b_abort.load(std::memory_order_relaxed)) break;

            last_score = score;
        }
    }
}
