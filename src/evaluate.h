#ifndef EVALUATE_H
#define EVALUATE_H

#include "board.h"

namespace Evaluation {
    // Piece value constants
    const int VAL_PAWN = 100;
    const int VAL_KNIGHT = 320;
    const int VAL_BISHOP = 330;
    const int VAL_ROOK = 500;
    const int VAL_QUEEN = 900;
    const int VAL_KING = 20000;

    // Master evaluation function returning score relative to the side to move
    int evaluate(const Board& board);
}

#endif // EVALUATE_H
