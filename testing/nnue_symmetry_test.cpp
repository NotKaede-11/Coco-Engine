#include "../src/board.h"
#include "../src/nnue.h"
#include "../src/movegen.h"
#include "../src/types.h"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cassert>
#include <memory>

// Vertically mirrors a FEN string (swapping color perspective, ranks, castling, and EP)
std::string mirror_fen(const std::string& fen) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream iss(fen);
    while (iss >> part) {
        parts.push_back(part);
    }
    if (parts.size() < 4) return fen;

    // 1. Piece placement
    std::string placement = parts[0];
    std::vector<std::string> ranks;
    std::istringstream pss(placement);
    std::string rank;
    while (std::getline(pss, rank, '/')) {
        ranks.push_back(rank);
    }

    // Reverse the order of the ranks
    std::reverse(ranks.begin(), ranks.end());

    // Swap piece color casing (uppercase <-> lowercase)
    for (auto& r : ranks) {
        for (char& c : r) {
            if (std::isupper(c)) {
                c = std::tolower(c);
            } else if (std::islower(c)) {
                c = std::toupper(c);
            }
        }
    }

    // Join ranks back together with '/'
    std::string new_placement = "";
    for (size_t i = 0; i < ranks.size(); ++i) {
        new_placement += ranks[i];
        if (i < ranks.size() - 1) {
            new_placement += "/";
        }
    }

    // 2. Side to move
    std::string new_stm = (parts[1] == "w") ? "b" : "w";

    // 3. Castling rights (swap White K/Q with Black k/q)
    std::string castling = parts[2];
    std::string new_castling = "";
    if (castling == "-") {
        new_castling = "-";
    } else {
        bool white_k = (castling.find('K') != std::string::npos);
        bool white_q = (castling.find('Q') != std::string::npos);
        bool black_k = (castling.find('k') != std::string::npos);
        bool black_q = (castling.find('q') != std::string::npos);

        if (black_k) new_castling += "K";
        if (black_q) new_castling += "Q";
        if (white_k) new_castling += "k";
        if (white_q) new_castling += "q";

        if (new_castling.empty()) new_castling = "-";
    }

    // 4. En passant target square (mirror vertically)
    std::string ep = parts[3];
    std::string new_ep = "-";
    if (ep != "-") {
        char file = ep[0];
        char rank_char = ep[1];
        char new_rank_char = (rank_char == '3') ? '6' : '3';
        new_ep = std::string(1, file) + new_rank_char;
    }

    // 5 & 6. Halfmove and Fullmove
    std::string halfmove = (parts.size() >= 5) ? parts[4] : "0";
    std::string fullmove = (parts.size() >= 6) ? parts[5] : "1";

    return new_placement + " " + new_stm + " " + new_castling + " " + new_ep + " " + halfmove + " " + fullmove;
}

int main() {
    std::cout << "=== Running NNUE Evaluation Symmetry Unit Test ===" << std::endl;

    // Initialize Zobrist keys (required by Board)
    Board::init_zobrist();
    init_all_attack_tables();

    // Load active network
    if (!g_nnue.load_network("coco.nnue")) {
        std::cerr << "FAILED: Could not load coco.nnue" << std::endl;
        return 1;
    }
    std::cout << "Successfully loaded coco.nnue" << std::endl;

    // Test positions
    std::vector<std::string> test_positions = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", // Startpos
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", // Position 4
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", // Position 3
        "8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1", // EP Check Position
        "r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1", // Castle Rights
        "4k3/1P6/8/8/8/8/8/4K3 w - - 0 1", // Passed Pawn Promotion
        "r1bqk1nr/pppp1ppp/2n5/2b1p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4" // Italian Game
    };

    bool all_passed = true;
    for (size_t i = 0; i < test_positions.size(); ++i) {
        const std::string& original_fen = test_positions[i];
        std::string mirrored_fen = mirror_fen(original_fen);

        auto board_orig = std::make_unique<Board>();
        board_orig->parse_fen(original_fen);

        auto board_mirror = std::make_unique<Board>();
        board_mirror->parse_fen(mirrored_fen);

        int score_orig = g_nnue.evaluate_nnue(*board_orig);
        int score_mirror = g_nnue.evaluate_nnue(*board_mirror);

        if (score_orig == score_mirror) {
            std::cout << "Position " << i << " PASSED! Score: " << score_orig << " | FEN: " << original_fen << std::endl;
        } else {
            std::cerr << "Position " << i << " FAILED!" << std::endl;
            std::cerr << "  Original FEN: " << original_fen << std::endl;
            std::cerr << "  Mirrored FEN: " << mirrored_fen << std::endl;
            std::cerr << "  Original Score: " << score_orig << std::endl;
            std::cerr << "  Mirrored Score: " << score_mirror << std::endl;
            all_passed = false;
        }
    }

    if (all_passed) {
        std::cout << "SUCCESS: All evaluation symmetry tests PASSED!" << std::endl;
        return 0;
    } else {
        std::cerr << "FAILURE: One or more evaluation symmetry tests FAILED!" << std::endl;
        return 1;
    }
}
