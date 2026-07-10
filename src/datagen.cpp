#include "datagen.h"
#include "board.h"
#include "types.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <memory>

// External function declarations from search.cpp
int alpha_beta(Board& board, int alpha, int beta, int depth, int ply, bool is_pv, bool in_null_move_search = false, int parent_eval_1 = INFINITY_SCORE, int parent_eval_2 = INFINITY_SCORE, Move excluded_move = Move(), int double_ext = 0);

// Enforce 32-byte memory packing layout required by bullet trainers
#pragma pack(push, 1)
struct BulletChessBoard {
    uint64_t occ;         // Piece occupancy bitboard map (8 bytes)
    uint8_t pcs[16];      // Squeezed piece feature indexing block (16 bytes)
    int16_t score;        // Engine evaluation score from perspective of side to move (2 bytes)
    uint8_t result;       // Final game outcome: 2 = Win (stm relative), 1 = Draw, 0 = Loss (stm relative) (1 byte)
    uint8_t ksq;          // Active side king square index (1 byte)
    uint8_t opp_ksq;      // Opponent king square index (1 byte)
    uint8_t extra[3];     // Zero-padding bytes to hit alignment target exactly (3 bytes)
};
#pragma pack(pop)

// Tracking buffers for in-flight positions prior to game termination
struct HarvestedPosition {
    BulletChessBoard packed;
    Color stm;
};

// Thread synchronization and global tracking variables
std::mutex file_mutex;
std::atomic<long long> global_positions_saved(0);
std::atomic<long long> global_total_games(0);

// Portable byte swapper for 64-bit integers
inline uint64_t swap_bytes(uint64_t v) {
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8)  |
           ((v & 0x000000FF00000000ULL) >> 8)  |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

// Check if side to move is in check
bool in_check(const Board& board) {
    Color us = board.get_side_to_move();
    int king_sq = 0;
    uint64_t king_bb = board.get_pieces(us, KING);
    if (king_bb > 0) {
        while ((king_bb & 1) == 0) {
            king_bb >>= 1;
            king_sq++;
        }
    }
    return board.is_square_attacked(king_sq, us ^ 1);
}

// Check if there are any legal capture moves available in the position
bool has_legal_captures(const Board& board, Board& temp_board) {
    MoveList list;
    generate_pseudo_legal_moves(const_cast<Board&>(board), list);
    for (int i = 0; i < list.count; ++i) {
        Move m = list.moves[i];
        if (m.is_capture()) {
            temp_board = board;
            if (temp_board.make_move(m)) {
                return true;
            }
        }
    }
    return false;
}

// Convert internal engine Board to 32-byte BulletChessBoard
void pack_board_state(const Board& board, int16_t score, float game_result_white, BulletChessBoard& cb) {
    uint64_t bbs[8];
    bbs[0] = board.get_occupancy(WHITE);
    bbs[1] = board.get_occupancy(BLACK);
    for (int pt = 0; pt < 6; ++pt) {
        bbs[2 + pt] = board.get_pieces(WHITE, pt) | board.get_pieces(BLACK, pt);
    }

    int stm = (board.get_side_to_move() == WHITE) ? 0 : 1;
    float result = game_result_white;

    if (stm == 1) { // Black is side to move, flip vertically
        for (int i = 0; i < 8; ++i) {
            bbs[i] = swap_bytes(bbs[i]);
        }
        // Swap White and Black occupancies
        uint64_t temp = bbs[0];
        bbs[0] = bbs[1];
        bbs[1] = temp;

        score = -score;
        result = 1.0f - result;
    }

    cb.occ = bbs[0] | bbs[1];
    std::memset(cb.pcs, 0, sizeof(cb.pcs));

    int idx = 0;
    uint64_t occ2 = cb.occ;
    while (occ2 > 0) {
        int sq = 0;
        uint64_t temp_occ = occ2;
        if (temp_occ > 0) {
            while ((temp_occ & 1) == 0) {
                temp_occ >>= 1;
                sq++;
            }
        }
        
        uint64_t bit = 1ULL << sq;
        occ2 &= occ2 - 1; // Clear LSB

        uint8_t color_bit = ((bit & bbs[1]) > 0) ? 8 : 0;
        uint8_t piece_type = 0;
        for (int pt = 0; pt < 6; ++pt) {
            if ((bit & bbs[2 + pt]) > 0) {
                piece_type = pt;
                break;
            }
        }

        uint8_t pc = color_bit | piece_type;
        cb.pcs[idx / 2] |= (pc << (4 * (idx & 1)));
        idx++;
    }

    cb.score = score;
    cb.result = (uint8_t)(2.0f * result + 0.5f);
    
    // Find king squares
    uint64_t stm_king = bbs[0] & bbs[7];
    int stm_ksq = 0;
    if (stm_king > 0) {
        while ((stm_king & 1) == 0) {
            stm_king >>= 1;
            stm_ksq++;
        }
    }
    cb.ksq = (uint8_t)stm_ksq;

    uint64_t opp_king = bbs[1] & bbs[7];
    int opp_ksq_val = 0;
    if (opp_king > 0) {
        while ((opp_king & 1) == 0) {
            opp_king >>= 1;
            opp_ksq_val++;
        }
    }
    cb.opp_ksq = (uint8_t)(opp_ksq_val ^ 56);

    cb.extra[0] = 0;
    cb.extra[1] = 0;
    cb.extra[2] = 0;
}

void datagen_worker(long long target, std::string output_path, int thread_id) {
    // Setup thread-isolated random distribution pipelines for opening choices
    std::mt19937 generator(std::random_device{}() + thread_id);
    
    // Allocate the large Board objects on the heap to prevent stack overflow
    auto board_ptr = std::make_unique<Board>();
    Board& board = *board_ptr;

    auto temp_board_ptr = std::make_unique<Board>();
    Board& temp_board = *temp_board_ptr;
    
    while (global_positions_saved.load() < target) {
        // Reset board to starting position
        board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        
        std::vector<HarvestedPosition> game_buffer;
        int game_ply = 0;
        float game_result = 0.5f; // Default to Draw
        
        // Execute a complete self-play game sequence loop
        while (game_ply < 250) {
            // Generate legal moves
            std::vector<Move> legal_moves;
            MoveList list;
            generate_pseudo_legal_moves(board, list);
            for (int i = 0; i < list.count; ++i) {
                Move m = list.moves[i];
                if (board.make_move(m)) {
                    legal_moves.push_back(m);
                    board.unmake_move(m);
                }
            }

            if (legal_moves.empty()) {
                if (in_check(board)) {
                    // Checkmate determined
                    game_result = (board.get_side_to_move() == WHITE) ? 0.0f : 1.0f;
                } else {
                    game_result = 0.5f; // Stalemate draw
                }
                break;
            }
            
            // Check for basic draw rule mitigations (repetition, 50-move rule, or king vs king)
            if (board.is_repetition() || board.get_halfmove_clock() >= 100 || count_bits(board.get_occupancy(2)) == 2) {
                game_result = 0.5f;
                break;
            }

            Move chosen_move;
            int eval_score = 0;

            if (game_ply < 8) {
                // Opening variance: random selection from top 3 moves
                std::vector<std::pair<Move, int>> scored_moves;
                for (const auto& m : legal_moves) {
                    temp_board = board;
                    temp_board.make_move(m);
                    int m_score = -alpha_beta(temp_board, -50000, 50000, 3, 1, false, false);
                    scored_moves.push_back({m, m_score});
                }
                
                // Sort moves based on score strength descending
                std::sort(scored_moves.begin(), scored_moves.end(), [](const auto& a, const auto& b) {
                    return a.second > b.second;
                });
                
                size_t selection_pool = std::min(size_t(3), scored_moves.size());
                std::uniform_int_distribution<size_t> dist(0, selection_pool - 1);
                int r = dist(generator);
                chosen_move = scored_moves[r].first;
                eval_score = scored_moves[r].second;
            } else {
                // Standard production play: search at depth 4
                eval_score = alpha_beta(board, -50000, 50000, 4, 0, true, false);
                
                // Retrieve best move from transposition table
                int temp_score;
                tt.probe(board.get_hash_key(), temp_score, chosen_move, 4, -50000, 50000, 0);

                if (chosen_move.is_none()) {
                    chosen_move = legal_moves[0];
                }
            }
            
            // Extract quiet states starting after the opening phase configuration boundary
            if (game_ply >= 8 && !in_check(board) && !has_legal_captures(board, temp_board)) {
                BulletChessBoard packed;
                pack_board_state(board, (int16_t)eval_score, 0.5f, packed); // 0.5f is dummy result
                game_buffer.push_back({packed, board.get_side_to_move()});
            }
            
            board.make_move(chosen_move);
            game_ply++;
        }
        
        // Game has terminated cleanly, batch-commit harvested positions using a thread lock
        if (!game_buffer.empty()) {
            std::lock_guard<std::mutex> lock(file_mutex);
            
            // Re-verify counter constraint under lock boundary protection
            if (global_positions_saved.load() >= target) break;
            
            // Assign final outcomes to all harvested positions
            for (auto& hb : game_buffer) {
                float res = game_result;
                if (hb.stm == BLACK) {
                    res = 1.0f - res;
                }
                hb.packed.result = (uint8_t)(2.0f * res + 0.5f);
            }

            std::ofstream out_file(output_path, std::ios::binary | std::ios::app);
            if (out_file.is_open()) {
                for (const auto& entry : game_buffer) {
                    out_file.write(reinterpret_cast<const char*>(&entry.packed), sizeof(BulletChessBoard));
                }
                out_file.close();
                
                global_positions_saved += game_buffer.size();
                global_total_games++;
            }
        }
    }
}

void run_datagen(long long target_positions, int num_threads, const std::string& output_path) {
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // Check if file already exists and determine resume offset
    long long existing_positions = 0;
    std::ifstream in_file(output_path, std::ios::binary | std::ios::ate);
    if (in_file.is_open()) {
        std::streampos size = in_file.tellg();
        existing_positions = size / sizeof(BulletChessBoard);
        in_file.close();
    }

    global_positions_saved.store(existing_positions, std::memory_order_relaxed);

    std::cout << "[Datagen] Configuration Rule Stacked -> Target: " << target_positions 
              << " | Active Worker Threads: " << num_threads << std::endl;

    if (existing_positions > 0) {
        std::cout << "[Datagen] Resuming from existing file. Detected " << existing_positions 
                  << " positions already saved." << std::endl;
        if (existing_positions >= target_positions) {
            std::cout << "[Datagen] Target already achieved!" << std::endl;
            return;
        }
    }
              
    // Ensure search timeout does not trigger
    Search::target_time = 0;
    Search::b_abort.store(false, std::memory_order_relaxed);

    std::vector<std::thread> workers;
    
    // Spawn configured worker threads natively
    for (int i = 0; i < num_threads; ++i) {
        workers.emplace_back(datagen_worker, target_positions, output_path, i);
    }
    
    long long last_reported_positions = existing_positions;
    // Main thread reporting loop monitors execution state metrics
    while (global_positions_saved.load() < target_positions) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        
        long long current_positions = global_positions_saved.load();
        auto current_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = current_time - start_time;
        
        if (current_positions >= last_reported_positions + 100000 || current_positions == target_positions) {
            double speed = (current_positions - existing_positions) / elapsed.count();
            double progress = (static_cast<double>(current_positions) / target_positions) * 100.0;
            
            std::cout << "[Datagen Progress] Harvested: " << current_positions << " / " << target_positions
                      << " | Speed: " << static_cast<long long>(speed) << " pos/sec"
                      << " | Progress: " << std::fixed << std::setprecision(2) << progress << "%"
                      << " | Games Evaluated: " << global_total_games.load() << std::endl;
                      
            last_reported_positions = current_positions;
        }
    }
    
    // Synchronize workers to guarantee data flush integrity prior to closeout
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_elapsed = end_time - start_time;
    
    std::cout << "\n[Datagen Complete] Successfully compiled " << global_positions_saved.load() 
              << " positions into " << output_path << std::endl;
    std::cout << "[Datagen Complete] Total Running Duration: " << total_elapsed.count() / 60.0 << " minutes." << std::endl;
}
