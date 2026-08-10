#include "../src/board.h"
#include "../src/evaluate.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

std::string color_mirror_fen(const std::string& fen) {
    std::istringstream input(fen);
    std::string placement, side, castling, ep, halfmove, fullmove;
    input >> placement >> side >> castling >> ep >> halfmove >> fullmove;
    char squares[64] = {};
    int rank = 7, file = 0;
    for (char ch : placement) {
        if (ch == '/') { --rank; file = 0; }
        else if (std::isdigit(static_cast<unsigned char>(ch))) file += ch - '0';
        else squares[rank * 8 + file++] = ch;
    }

    char mirrored[64] = {};
    for (int square = 0; square < 64; ++square) {
        if (!squares[square]) continue;
        char piece = squares[square];
        mirrored[square ^ 56] = std::islower(static_cast<unsigned char>(piece))
            ? static_cast<char>(std::toupper(static_cast<unsigned char>(piece)))
            : static_cast<char>(std::tolower(static_cast<unsigned char>(piece)));
    }

    std::ostringstream output;
    for (rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (file = 0; file < 8; ++file) {
            char piece = mirrored[rank * 8 + file];
            if (!piece) { ++empty; continue; }
            if (empty) { output << empty; empty = 0; }
            output << piece;
        }
        if (empty) output << empty;
        if (rank) output << '/';
    }

    std::string mirrored_castling;
    if (castling != "-") {
        if (castling.find('k') != std::string::npos) mirrored_castling += 'K';
        if (castling.find('q') != std::string::npos) mirrored_castling += 'Q';
        if (castling.find('K') != std::string::npos) mirrored_castling += 'k';
        if (castling.find('Q') != std::string::npos) mirrored_castling += 'q';
    }
    if (mirrored_castling.empty()) mirrored_castling = "-";
    std::string mirrored_ep = ep;
    if (ep != "-") mirrored_ep[1] = static_cast<char>('1' + (7 - (ep[1] - '1')));
    output << ' ' << (side == "w" ? "b" : "w") << ' ' << mirrored_castling
           << ' ' << mirrored_ep << ' ' << halfmove << ' ' << fullmove;
    return output.str();
}

Evaluation::HceTrace check(const char* fen) {
    Board board;
    board.parse_fen(fen);
    Evaluation::HceTrace trace;
    const int score = Evaluation::evaluate_hce(board, &trace);
    if (score != trace.side_to_move_score) {
        std::cerr << "HCE trace mismatch: " << fen << '\n';
        std::exit(1);
    }
    const int mg = trace.material_mg + trace.psqt_mg + trace.pawns_mg
        + trace.imbalance_mg + trace.passers_mg + trace.threats_mg
        + trace.mobility_mg + trace.king_safety_mg + trace.endgame_mg + trace.tempo_mg;
    const int eg = trace.material_eg + trace.psqt_eg + trace.pawns_eg
        + trace.imbalance_eg + trace.passers_eg + trace.threats_eg
        + trace.mobility_eg + trace.king_safety_eg + trace.endgame_eg + trace.tempo_eg;
    const int tapered = (mg * trace.phase + eg * (24 - trace.phase)) / 24;
    const int recomposed = trace.exact_draw ? 0 : tapered * trace.scale / 128;
    const int expected = board.get_side_to_move() == WHITE ? recomposed : -recomposed;
    if (score != expected) {
        std::cerr << "HCE recomposition mismatch: " << fen << '\n';
        std::exit(1);
    }
    Evaluation::HceTrace repeat;
    if (Evaluation::evaluate_hce(board, &repeat) != score
        || repeat.white_score != trace.white_score
        || repeat.side_to_move_score != trace.side_to_move_score) {
        std::cerr << "HCE repeatability mismatch: " << fen << '\n';
        std::exit(1);
    }
    return trace;
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "HCE invariant failed: " << message << '\n';
        std::exit(1);
    }
}

void check_color_mirror(const Board& board) {
    const std::string fen = board.get_fen();
    Board mirrored;
    mirrored.parse_fen(color_mirror_fen(fen));
    Evaluation::HceTrace original_trace, mirrored_trace;
    const int original = Evaluation::evaluate_hce(board, &original_trace);
    const int reflected = Evaluation::evaluate_hce(mirrored, &mirrored_trace);
    if (original != reflected) {
        std::cerr << "HCE color-mirror mismatch: " << original << " versus " << reflected
                  << "\n  original: " << fen
                  << "\n  mirrored: " << mirrored.get_fen() << '\n';
        std::exit(1);
    }
}

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    if (!g_nnue.load_network("coco.nnue")) return 1;
    const auto initial_white = check("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const auto initial_black = check("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");
    require(initial_white.side_to_move_score > 0 && initial_black.side_to_move_score > 0,
            "tempo must favor either side to move in a symmetric position");
    require(initial_white.side_to_move_score == initial_black.side_to_move_score,
            "symmetric side-to-move tempo must have equal magnitude");

    const auto middlegame = check("r3k2r/pp1n1p1p/2pbpn2/q5p1/2BPP3/2N2N2/PPQ2PPP/R4RK1 w kq - 3 11");
    require(middlegame.phase > 0 && middlegame.phase < 24, "middlegame phase must be tapered");
    require(middlegame.mobility_mg != 0 || middlegame.mobility_eg != 0,
            "mobility features must activate");
    require(middlegame.king_safety_mg != 0 || middlegame.king_safety_eg != 0,
            "king-safety features must activate");

    const auto pawn_structure = check("4k3/pp4pp/3P4/8/8/8/PP4PP/4K3 w - - 0 1");
    require(pawn_structure.pawns_mg != 0 || pawn_structure.pawns_eg != 0,
            "pawn-structure features must activate");
    require(pawn_structure.passers_mg != 0 || pawn_structure.passers_eg != 0,
            "passed-pawn features must activate");

    const auto insufficient = check("4k3/8/8/8/8/8/8/2B1K3 w - - 0 1");
    require(insufficient.exact_draw && insufficient.white_score == 0,
            "K+B versus K must be an exact draw");

    const auto opposite_bishops = check("4k3/4b3/7p/8/8/P7/2B5/4K3 w - - 0 1");
    require(!opposite_bishops.exact_draw && opposite_bishops.scale < 128,
            "opposite-coloured bishops must use the drawish scale");

    check("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    check("4k3/8/8/8/3Q4/8/8/4K3 b - - 0 1");

    Board random_board;
    random_board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    std::mt19937 generator(0xC0C05EEDu);
    for (int sample = 0; sample < 250; ++sample) {
        check_color_mirror(random_board);
        MoveList pseudo;
        generate_pseudo_legal_moves(random_board, pseudo);
        std::vector<Move> legal;
        const LegalityMasks masks = random_board.get_legality_masks();
        for (int index = 0; index < pseudo.count; ++index)
            if (random_board.is_move_legal(pseudo.moves[index], masks)) legal.push_back(pseudo.moves[index]);
        if (legal.empty()) {
            random_board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
            continue;
        }
        const Move selected = legal[generator() % legal.size()];
        if (!random_board.make_move(selected, true)) {
            std::cerr << "legal random move rejected during HCE symmetry test\n";
            return 1;
        }
        if ((sample + 1) % 80 == 0)
            random_board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    }
    std::cout << "PASS: HCE T2-T5 trace, scaling, and feature invariants\n";
    return 0;
}
