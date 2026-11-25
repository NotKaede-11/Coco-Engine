#ifndef DATAGEN_GAME_HISTORY_H
#define DATAGEN_GAME_HISTORY_H

#include "board.h"
#include "movegen.h"
#include <algorithm>
#include <vector>

// Game-level repetition belongs to played positions, not the search's
// speculative make/unmake or null-move stack. A FEN supplies one occurrence;
// unknown moves before a supplied FEN cannot be reconstructed from its clocks.
class DatagenGameHistory {
public:
    DatagenGameHistory() { keys.reserve(513); }

    void reset(const Board& board) {
        keys.clear();
        record(board);
    }

    // Call exactly once after a successful played move (also used by reset).
    void record(const Board& board) {
        if (board.get_halfmove_clock() == 0) keys.clear();
        keys.push_back(position_key(board));
    }

    bool is_threefold() const {
        return !keys.empty()
            && std::count(keys.begin(), keys.end(), keys.back()) >= 3;
    }

private:
    std::vector<U64> keys;

    static U64 position_key(const Board& board) {
        U64 key = board.get_hash_key();
        const int ep = board.get_en_passant_square();
        if (ep == SQ_NONE) return key;

        // Coco's search key includes even an unusable EP target. For game
        // repetition, retain that component only if an EP capture is legal.
        MoveList moves;
        generate_pseudo_legal_moves(board, moves);
        const auto masks = board.get_legality_masks();
        for (int i = 0; i < moves.count; ++i)
            if (moves.moves[i].is_en_passant()
                && board.is_move_legal(moves.moves[i], masks)) return key;
        return key ^ Board::zobrist_ep[ep % 8];
    }
};

#endif
