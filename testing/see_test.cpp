#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include <cstdlib>
#include <iostream>
#include <string>

Move find_move(Board& board, const std::string& uci) {
    MoveList moves;
    generate_pseudo_legal_moves(board, moves);
    for (int i = 0; i < moves.count; ++i) {
        const Move move = moves.moves[i];
        std::string text = square_to_str(move.from()) + square_to_str(move.to());
        if (move.is_promotion()) {
            const char suffix[] = "?nbrq";
            text += suffix[move.promotion_piece_type()];
        }
        if (text == uci) return move;
    }
    return Move();
}

void expect_see(const char* fen, const char* uci, int expected) {
    Board board;
    board.parse_fen(fen);
    const Move move = find_move(board, uci);
    if (move.is_none()) {
        std::cerr << "missing move " << uci << " in " << fen << '\n';
        std::exit(1);
    }
    const int actual = board.see(move);
    if (actual != expected) {
        std::cerr << "SEE " << uci << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    if (!g_nnue.load_network("coco.nnue")) return 1;

    expect_see("4k3/8/2p5/3p4/4P3/8/8/4K3 w - - 0 1", "e4d5", 0);
    expect_see("3rk3/8/8/3p4/8/8/8/3QK3 w - - 0 1", "d1d5", -800);
    expect_see("3k4/8/8/3pP3/8/8/8/3rK3 w - d6 0 1", "e5d6", 0);
    expect_see("k7/6P1/8/8/8/8/6r1/7K w - - 0 1", "g7g8q", -100);

    std::cout << "PASS: SEE captures, EP x-ray, attacker removal, and promotion value\n";
    return 0;
}
