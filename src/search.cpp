#include "search.h"
#include "types.h"
#include "movegen.h"
#include "tt.h"
#include "evaluate.h"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>

// Constants for evaluation and search bounds
const int INFINITY_SCORE = 50000;
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
    if (Search::hard_limit != 0 && (nodes_visited & 2047) == 0) {
        uint64_t elapsed = get_time_ms() - Search::start_time;
        if (elapsed >= Search::hard_limit) {
            Search::b_abort = true;
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
thread_local int history_table[2][7][64]; // [color][piece_type][to_square] - size 7 to prevent out-of-bounds on NO_PIECE_TYPE
int lmr_table[64][64];

// Move Ordering helper function using stack memory
void order_moves(const Board& board, MoveList& move_list, Move tt_move, int ply) {
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
            
            if (mvv_lva_values[victim] >= mvv_lva_values[assailant]) {
                scores[i] = 1000000 + capture_value;
            } else {
                scores[i] = 10000 + capture_value;
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
                
                // Safety bound check
                if (piece < 7) {
                    scores[i] = history_table[side][piece][to];
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
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, bool is_pv, bool in_null_move_search = false);
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
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, bool is_pv, bool in_null_move_search) {
    // Cooperative search abortion check
    check_time();
    if (Search::b_abort) return 0;
    
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
    int king_sq = get_lsb(board.get_pieces(us, KING));
    bool in_check = board.is_square_attacked(king_sq, us ^ 1);
    
    // Null Move Pruning (NMP)
    if (depth >= 3 && !is_pv && !in_check && !in_null_move_search) {
        bool has_non_pawn_material = board.get_pieces(us, KNIGHT) || 
                                     board.get_pieces(us, BISHOP) || 
                                     board.get_pieces(us, ROOK) || 
                                     board.get_pieces(us, QUEEN);
        if (has_non_pawn_material) {
            int R = Search::NMP_Base + (depth / Search::NMP_Divisor);
            board.make_null_move();
            int null_score = -alpha_beta(board, -beta, -beta + 1, depth - 1 - R, ply + 1, false, true);
            board.unmake_null_move();
            
            if (null_score >= beta) {
                return beta;
            }
        }
    }
    
    // Probe the Transposition Table
    int tt_score = 0;
    Move tt_move;
    if (tt.probe(board.get_hash_key(), tt_score, tt_move, depth, alpha, beta, ply)) {
        return tt_score;
    }
    
    // Retrieve static evaluation
    int static_eval = Evaluation::evaluate(board);
    
    // Reverse Futility Pruning (RFP) / Static Null Move Pruning
    // Strictly disabled if beta is near a checkmate score to prevent mate-blindness tactical leaks
    if (depth <= 3 && !is_pv && !in_check && std::abs(beta) < MATE_SCORE - MAX_PLY) {
        int margin = Search::RFP_Margin * depth;
        if (static_eval - margin >= beta) {
            return static_eval;
        }
    }
    
    // Razoring
    if (depth == 1 && !is_pv && !in_check && alpha > -INFINITY_SCORE + 1000) {
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
        alpha_beta(board, alpha, beta, iid_depth, ply, is_pv, in_null_move_search);
        // Time-abort check: immediately return 0 and skip probing the TT to prevent move pollution
        if (Search::b_abort) return 0;
        int dummy_score;
        tt.probe(board.get_hash_key(), dummy_score, tt_move, 0, -INFINITY_SCORE, INFINITY_SCORE, ply);
    }
    
    int alpha_orig = alpha;
    
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    order_moves(board, move_list, tt_move, ply);
    
    int legal_moves_count = 0;
    int best_score = -INFINITY_SCORE;
    Move best_move_in_node;
    int moves_searched = 0;
    
    for (int i = 0; i < move_list.count; i++) {
        Move move = move_list.moves[i];
        
        // Late Move Pruning (LMP)
        if (!move.is_capture() && !move.is_promotion()) {
            if (depth <= 3 && !is_pv && !in_check) {
                int move_threshold = 4 + (depth * depth);
                if (moves_searched >= move_threshold) {
                    break;
                }
            }
        }
        
        // Move-Level Futility Pruning
        // Strictly disabled if alpha is near a checkmate score to prevent mate-blindness tactical leaks
        if (depth <= 2 && !in_check && !move.is_capture() && !move.is_promotion()) {
            if (move != tt_move && 
                (ply >= MAX_PLY || (move != killer_moves[ply][0] && move != killer_moves[ply][1])) &&
                std::abs(alpha) < MATE_SCORE - MAX_PLY) {
                int margin = 100 * depth;
                if (static_eval + margin <= alpha) {
                    continue;
                }
            }
        }
        
        // Symmetrical illegal move filtering
        if (!board.make_move(move)) {
            continue;
        }
        legal_moves_count++;
        
        // Check if this move gives check
        Color them = board.get_side_to_move();
        int them_king_sq = get_lsb(board.get_pieces(them, KING));
        bool gives_check = board.is_square_attacked(them_king_sq, them ^ 1);
        
        int score;
        if (moves_searched == 0) {
            score = -alpha_beta(board, -beta, -alpha, depth - 1, ply + 1, is_pv, in_null_move_search);
        } else {
            bool lmr_applied = false;
            
            // Late Move Reductions (LMR) - Exclude PV nodes
            if (depth >= 3 && !move.is_capture() && !move.is_promotion() && !in_check &&
                move != tt_move && !gives_check && moves_searched >= 4 && !is_pv) {
                
                int d_idx = std::min(depth, 63);
                int m_idx = std::min(moves_searched, 63);
                int reduction = lmr_table[d_idx][m_idx];
                
                if (depth - 1 - reduction < 1) {
                    reduction = depth - 2; // Cap reduction so remaining depth is exactly 1
                }
                
                score = -alpha_beta(board, -alpha - 1, -alpha, depth - 1 - reduction, ply + 1, false, in_null_move_search);
                lmr_applied = true;
            } else {
                score = -alpha_beta(board, -alpha - 1, -alpha, depth - 1, ply + 1, false, in_null_move_search);
            }
            
            // Re-search trigger
            if (score > alpha && score < beta) {
                score = -alpha_beta(board, -beta, -alpha, depth - 1, ply + 1, is_pv, in_null_move_search);
            }
        }
        
        board.unmake_move(move);
        
        // Safely abort this recursive branch
        if (Search::b_abort) return 0;
        
        // Record quiet moves that did not trigger a cutoff
        if (score < beta) {
            if (!move.is_capture() && !move.is_promotion() && quiet_count < 64) {
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
                tt.store(board.get_hash_key(), move, beta, depth, HASH_BETA, ply);
                
                // Update killer moves and history heuristic for quiet moves
                if (!move.is_capture() && !move.is_promotion() && ply < MAX_PLY) {
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
                        history_table[side][piece][to] += depth * depth;
                        
                        // Apply malus loop for unsuccessful quiet moves
                        for (int i = 0; i < quiet_count; i++) {
                            Move failed_move = quiet_moves_searched[i];
                            Piece failed_piece_raw = board.get_piece_at(failed_move.from());
                            PieceType failed_piece = (PieceType)(failed_piece_raw % 6);
                            int failed_to_sq = failed_move.to();
                            if (failed_piece < 7) {
                                history_table[side][failed_piece][failed_to_sq] -= depth * depth;
                            }
                        }
                        
                        // Prevent overflow via aging mechanism checking absolute values safely
                        if (history_table[side][piece][to] > Search::History_Threshold || history_table[side][piece][to] < -Search::History_Threshold) {
                            for (int c = 0; c < 2; ++c) {
                                for (int p = 0; p < 7; ++p) {
                                    for (int s = 0; s < 64; ++s) {
                                        history_table[c][p][s] /= 2;
                                    }
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
    tt.store(board.get_hash_key(), best_move_in_node, best_score, depth, flag, ply);
    
    return best_score;
}

// Quiescence Search
int quiescence(Board& board, int alpha, int beta, int ply) {
    check_time();
    if (Search::b_abort) return 0;
    
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
    
    for (int i = 0; i < move_list.count; i++) {
        Move move = move_list.moves[i];
        
        // Quiescence search must only evaluate capture sequences
        if (!move.is_capture()) {
            continue;
        }
        
        if (!board.make_move(move)) {
            continue;
        }
        
        int score = -quiescence(board, -beta, -alpha, ply + 1);
        board.unmake_move(move);
        
        if (Search::b_abort) return 0;
        
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
    bool b_abort = false;

    int RFP_Margin = 74;
    int LMR_Constant_Scaled = 224;
    int NMP_Base = 2;
    int NMP_Divisor = 6;
    int Aspiration_Delta = 18;
    int History_Threshold = 16367;

    void init_search_tables() {
        for (int d = 1; d < 64; d++) {
            for (int m = 1; m < 64; m++) {
                lmr_table[d][m] = static_cast<int>(0.5 + log(d) * log(m) / (static_cast<double>(LMR_Constant_Scaled) / 100.0));
            }
        }
    }

    void allocate_time(int time_left, int increment) {
        const int OVERHEAD_BUFFER = 30;

        int usable_time = time_left - OVERHEAD_BUFFER;
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

    // Master search control loop with Iterative Deepening
    void search_position(Board& board, int max_depth, int wtime, int btime, int winc, int binc, int movetime) {
        start_time = get_time_ms();
        b_abort = false;
        
        // Reset killer moves and history table at start of search
        for (int i = 0; i < MAX_PLY; ++i) {
            killer_moves[i][0] = Move();
            killer_moves[i][1] = Move();
        }
        for (int c = 0; c < 2; ++c) {
            for (int p = 0; p < 7; ++p) {
                for (int s = 0; s < 64; ++s) {
                    history_table[c][p][s] = 0;
                }
            }
        }
        
        // Determine active side timing configurations
        if (movetime > 0) {
            // Fixed movetime: respect the GUI limit, but stay under python-chess' 60s timeout.
            hard_limit = static_cast<uint64_t>(std::min(movetime, 55000));
            soft_limit = hard_limit;
            target_time = start_time + hard_limit;
        } else {
            int my_time = (board.get_side_to_move() == WHITE) ? wtime : btime;
            int my_inc = (board.get_side_to_move() == WHITE) ? winc : binc;
            
            if (my_time > 0) {
                allocate_time(my_time, my_inc);
                target_time = start_time + hard_limit;
            } else {
                soft_limit = 0;
                hard_limit = 0;
                target_time = 0; // Infinite/fixed-depth search
            }
        }
        
        Move best_move;
        Move last_completed_best_move;
        
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
                    if (b_abort) {
                        break;
                    }
                    
                    score = alpha_beta(board, alpha, beta, current_depth, 0, true, false);
                    
                    if (b_abort) {
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
            if (b_abort) {
                break;
            }
            
            last_score = score;
            
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
            
            // Calculate metrics
            uint64_t elapsed = get_time_ms() - start_time;
            uint64_t nps = elapsed > 0 ? (nodes_visited * 1000) / elapsed : nodes_visited * 1000;
            
            // Print standardized UCI info line
            std::cout << "info depth " << current_depth 
                      << " seldepth " << max_ply_reached << " ";
            
            if (score > MATE_THRESHOLD) {
                int plies = MATE_SCORE - score;
                std::cout << "score mate " << (plies + 1) / 2;
            } else if (score < -MATE_THRESHOLD) {
                int plies = score + MATE_SCORE;
                std::cout << "score mate " << -((plies + 1) / 2);
            } else {
                std::cout << "score cp " << score;
            }
            
            std::cout << " nodes " << nodes_visited 
                      << " time " << elapsed 
                      << " nps " << nps 
                      << " pv";
                      
            for (int i = 0; i < pv_len; i++) {
                std::cout << " " << move_to_str(pv[i]);
            }
            std::cout << std::endl;

            if (soft_limit != 0 && elapsed > soft_limit) {
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
        std::cout << "bestmove " << move_to_str(last_completed_best_move) << std::endl;
    }
}
