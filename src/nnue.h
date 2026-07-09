#ifndef NNUE_H
#define NNUE_H

#include "board.h"
#include <string>
#include <cstdint>
#include <immintrin.h>

// Inline perspective feature mapping helper functions
inline int get_feature_index_white(int color, int pt, int sq) {
    if (color == 0) { // WHITE is 0
        return pt * 64 + sq;
    } else {
        return (pt + 6) * 64 + sq;
    }
}

inline int get_feature_index_black(int color, int pt, int sq) {
    int sq_flipped = sq ^ 56;
    if (color == 1) { // BLACK is 1
        return pt * 64 + sq_flipped;
    } else {
        return (pt + 6) * 64 + sq_flipped;
    }
}

class NNUEEvaluator {
public:
    NNUEEvaluator();

    // Loads the quantized NNUE weights and biases from a binary file.
    // Verifies that the file size is exactly 394,244 bytes.
    bool load_network(const std::string& filename);

    // Performs the forward pass evaluation of the neural network
    // using the pre-calculated accumulator state and returns a score
    // in centipawns relative to the side to move.
    int evaluate_nnue(const Board& board) const;

    // Initializes the accumulator state from scratch for a given board position.
    void init_accumulator(const Board& board, Accumulator& acc) const;

    // Incremental activation helper
    inline void accumulator_activate(Accumulator& acc, int side, int feature_index) const {
        const int16_t* weights = layer1_weights[feature_index];
        for (int i = 0; i < 256; ++i) {
            acc.v[side][i] += weights[i];
        }
    }

    // Incremental deactivation helper
    inline void accumulator_deactivate(Accumulator& acc, int side, int feature_index) const {
        const int16_t* weights = layer1_weights[feature_index];
        for (int i = 0; i < 256; ++i) {
            acc.v[side][i] -= weights[i];
        }
    }

private:
    // NNUE Model parameters (matching the 394,756-byte binary layout)
    int16_t layer1_weights[768][256];
    int16_t layer1_biases[256];
    int16_t layer2_weights[512];
    int32_t layer2_bias;
};

// Global NNUE evaluator instance
extern NNUEEvaluator g_nnue;

#endif // NNUE_H
