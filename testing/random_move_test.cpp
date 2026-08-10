#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/types.h"
#include <iostream>
#include <vector>
#include <random>
#include <string>
#include <sstream>
#include <cassert>
#include <memory>
#include <array>

bool verify_staged_move_lists(const Board& board, const MoveList& all_moves) {
    MoveList captures, quiets, noisy, evasions;
    generate_capture_moves(board, captures);
    generate_quiet_moves(board, quiets);
    generate_noisy_moves(board, noisy);
    generate_evasion_moves(board, evasions);

    std::array<bool, 65536> all{};
    std::array<bool, 65536> seen{};
    for (int i = 0; i < all_moves.count; ++i) {
        if (all[all_moves.moves[i].value]) return false;
        all[all_moves.moves[i].value] = true;
    }

    auto verify = [&](const MoveList& list, auto expected) {
        seen.fill(false);
        for (int i = 0; i < list.count; ++i) {
            const Move move = list.moves[i];
            if (seen[move.value] || !all[move.value] || !expected(move)) return false;
            seen[move.value] = true;
        }
        for (int i = 0; i < all_moves.count; ++i) {
            const Move move = all_moves.moves[i];
            if (expected(move) != seen[move.value]) return false;
        }
        return true;
    };

    return verify(captures, [](Move move) { return move.is_capture(); })
        && verify(quiets, [](Move move) { return !move.is_capture(); })
        && verify(noisy, [](Move move) { return move.is_capture() || move.is_promotion(); })
        && verify(evasions, [](Move) { return true; });
}

// Move string converter helper
std::string test_move_to_str(Move m) {
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

// Compare two accumulators for exact equality
bool accumulators_equal(const Accumulator& a, const Accumulator& b) {
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < L1_SIZE; ++i) {
            if (a.v[side][i] != b.v[side][i]) {
                return false;
            }
        }
    }
    return true;
}

// Verify that the king of the side to move is in check
bool is_in_check(const Board& board) {
    Color us = board.get_side_to_move();
    // Find our king square
    int king_sq = -1;
    for (int sq = 0; sq < 64; ++sq) {
        Piece pc = board.get_piece_at(sq);
        if (pc != NO_PIECE && pc % 6 == KING && pc / 6 == us) {
            king_sq = sq;
            break;
        }
    }
    if (king_sq == -1) return false;
    return board.is_square_attacked(king_sq, us ^ 1);
}

int main(int argc, char** argv) {
    std::cout << "=== Running Random-Move Game Simulation Stress-Test ===" << std::endl;

    // 1. Initialize engine systems
    Board::init_zobrist();
    init_all_attack_tables();

    // Load neural network
    if (!g_nnue.load_network("coco.nnue")) {
        std::cerr << "FAILED: Could not load coco.nnue" << std::endl;
        return 1;
    }
    std::cout << "Successfully loaded coco.nnue" << std::endl;

    // Set up random number generator
    uint32_t seed = argc >= 3 ? static_cast<uint32_t>(std::stoul(argv[2])) : 0xC0C014u;
    std::mt19937 gen(seed);

    int total_games = argc >= 2 ? std::stoi(argv[1]) : 5000;
    if (total_games <= 0) {
        std::cerr << "Game count must be positive" << std::endl;
        return 1;
    }
    std::cout << "Games: " << total_games << ", seed: " << seed << std::endl;
    int checkmate_count = 0;
    int stalemate_count = 0;
    int draw_50mr_count = 0;
    int draw_repetition_count = 0;
    int ply_limit_count = 0;
    uint64_t total_plies_searched = 0;

    for (int g = 1; g <= total_games; ++g) {
        auto board = std::make_unique<Board>();
        board->parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

        int plies = 0;
        bool game_over = false;

        while (plies < 250 && !game_over) {
            std::string fen_before = board->get_fen();
            U64 hash_before = board->get_hash_key();
            Accumulator acc_before = board->get_accumulator();

            // Generate pseudo-legal moves
            MoveList pseudo_moves;
            generate_pseudo_legal_moves(*board, pseudo_moves);
            if (!verify_staged_move_lists(*board, pseudo_moves)) {
                std::cerr << "CRITICAL ERROR: staged move-list mismatch at " << board->get_fen() << std::endl;
                return 1;
            }

            // Filter for legal moves
            std::vector<Move> legal_moves;
            for (int i = 0; i < pseudo_moves.count; ++i) {
                Move m = pseudo_moves.moves[i];
                if (board->make_move(m)) {
                    legal_moves.push_back(m);
                    board->unmake_move(m);
                }
            }

            // Check game termination conditions
            if (legal_moves.empty()) {
                if (is_in_check(*board)) {
                    checkmate_count++;
                } else {
                    stalemate_count++;
                }
                game_over = true;
                break;
            }

            // Check 50-move rule
            if (board->get_halfmove_clock() >= 100) {
                draw_50mr_count++;
                game_over = true;
                break;
            }

            // Check repetition draw
            if (board->is_repetition()) {
                draw_repetition_count++;
                game_over = true;
                break;
            }

            // Pick a random legal move
            std::uniform_int_distribution<> dis(0, legal_moves.size() - 1);
            Move chosen_move = legal_moves[dis(gen)];

            // Play the move
            bool make_success = board->make_move(chosen_move);
            assert(make_success && "make_move failed on verified legal move!");

            // Sanity Check 1: Verify incremental accumulator update matches re-init
            Accumulator acc_current = board->get_accumulator();
            Accumulator acc_expected;
            g_nnue.init_accumulator(*board, acc_expected);
            if (!accumulators_equal(acc_current, acc_expected)) {
                std::cerr << "CRITICAL ERROR: Accumulator mismatch!" << std::endl;
                std::cerr << "  Game: " << g << ", Ply: " << plies << std::endl;
                std::cerr << "  FEN before: " << fen_before << std::endl;
                std::cerr << "  Chosen Move: " << test_move_to_str(chosen_move) << std::endl;
                std::cerr << "  FEN after: " << board->get_fen() << std::endl;
                return 1;
            }

            // Unmake the move to check reversibility
            board->unmake_move(chosen_move);

            // Sanity Check 2: Verify state is 100% restored after unmake
            std::string fen_after_unmake = board->get_fen();
            U64 hash_after_unmake = board->get_hash_key();
            Accumulator acc_after_unmake = board->get_accumulator();

            if (fen_before != fen_after_unmake) {
                std::cerr << "CRITICAL ERROR: FEN mismatch after unmake_move!" << std::endl;
                std::cerr << "  Before: " << fen_before << std::endl;
                std::cerr << "  After:  " << fen_after_unmake << std::endl;
                return 1;
            }
            if (hash_before != hash_after_unmake) {
                std::cerr << "CRITICAL ERROR: Zobrist hash mismatch after unmake_move!" << std::endl;
                std::cerr << "  Before: " << hash_before << std::endl;
                std::cerr << "  After:  " << hash_after_unmake << std::endl;
                return 1;
            }
            if (!accumulators_equal(acc_before, acc_after_unmake)) {
                std::cerr << "CRITICAL ERROR: Accumulator mismatch after unmake_move!" << std::endl;
                return 1;
            }

            // Play the move again to continue the game
            board->make_move(chosen_move);

            plies++;
            total_plies_searched++;
        }

        if (plies >= 250) {
            ply_limit_count++;
        }

        if (g % 1000 == 0) {
            std::cout << "Completed " << g << " games..." << std::endl;
        }
    }

    std::cout << "\n=== Stress Test Results ===" << std::endl;
    std::cout << "Total Games Played: " << total_games << std::endl;
    std::cout << "Total Plies Searched: " << total_plies_searched << std::endl;
    std::cout << "Checkmates: " << checkmate_count << std::endl;
    std::cout << "Stalemates: " << stalemate_count << std::endl;
    std::cout << "50-Move Draw Rules: " << draw_50mr_count << std::endl;
    std::cout << "Repetition Draws: " << draw_repetition_count << std::endl;
    std::cout << "Ply Limit (250+) reached: " << ply_limit_count << std::endl;
    std::cout << "SUCCESS: Random game stress test completed with ZERO errors!" << std::endl;

    return 0;
}
