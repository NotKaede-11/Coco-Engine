#ifndef NNUE_H
#define NNUE_H

#include "board.h"
#include "types.h"
#include <string>
#include <cstdint>
#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

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

    // Loads neural network weights from a file
    bool load_network(const std::string& filename);

    // Evaluates the board position from scratch using NNUE.
    int evaluate_nnue(const Board& board) const;

    // Initializes the accumulator state from scratch for a given board position.
    void init_accumulator(const Board& board, Accumulator& acc) const;

    // Incremental activation helper
    inline void accumulator_activate(Accumulator& acc, int side, int feature_index) const {
        const int16_t* weights = layer1_weights[feature_index];
#if defined(__AVX2__)
        for (int i = 0; i < L1_SIZE; i += 16) {
            __m256i acc_val = _mm256_load_si256((const __m256i*)&acc.v[side][i]);
            __m256i w_val = _mm256_load_si256((const __m256i*)&weights[i]);
            __m256i res = _mm256_add_epi16(acc_val, w_val);
            _mm256_store_si256((__m256i*)&acc.v[side][i], res);
        }
#elif defined(__ARM_NEON)
        for (int i = 0; i < L1_SIZE; i += 8) {
            int16x8_t acc_val = vld1q_s16(&acc.v[side][i]);
            int16x8_t w_val = vld1q_s16(&weights[i]);
            vst1q_s16(&acc.v[side][i], vaddq_s16(acc_val, w_val));
        }
#else
        for (int i = 0; i < L1_SIZE; ++i) {
            acc.v[side][i] += weights[i];
        }
#endif
    }

    // Incremental deactivation helper
    inline void accumulator_deactivate(Accumulator& acc, int side, int feature_index) const {
        const int16_t* weights = layer1_weights[feature_index];
#if defined(__AVX2__)
        for (int i = 0; i < L1_SIZE; i += 16) {
            __m256i acc_val = _mm256_load_si256((const __m256i*)&acc.v[side][i]);
            __m256i w_val = _mm256_load_si256((const __m256i*)&weights[i]);
            __m256i res = _mm256_sub_epi16(acc_val, w_val);
            _mm256_store_si256((__m256i*)&acc.v[side][i], res);
        }
#elif defined(__ARM_NEON)
        for (int i = 0; i < L1_SIZE; i += 8) {
            int16x8_t acc_val = vld1q_s16(&acc.v[side][i]);
            int16x8_t w_val = vld1q_s16(&weights[i]);
            vst1q_s16(&acc.v[side][i], vsubq_s16(acc_val, w_val));
        }
#else
        for (int i = 0; i < L1_SIZE; ++i) {
            acc.v[side][i] -= weights[i];
        }
#endif
    }

private:
    void load_embedded_network();

    // NNUE Model parameters
    alignas(32) int16_t layer1_weights[768][L1_SIZE];
    alignas(32) int16_t layer1_biases[L1_SIZE];
    alignas(32) int16_t layer2_weights[2 * L1_SIZE];
    int32_t layer2_bias;
};

// Global NNUE evaluator instance
extern NNUEEvaluator g_nnue;

#endif // NNUE_H
