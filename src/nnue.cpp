#include "nnue.h"
#include <iostream>
#include <cstdio>
#include <memory>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
static std::string get_executable_directory() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string path_str(path);
    size_t pos = path_str.find_last_of("\\/");
    return (pos == std::string::npos) ? "" : path_str.substr(0, pos + 1);
}
#else
#include <unistd.h>
#include <limits.h>
static std::string get_executable_directory() {
    char path[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", path, PATH_MAX);
    std::string path_str = (count > 0) ? std::string(path, count) : "";
    size_t pos = path_str.find_last_of("/");
    return (pos == std::string::npos) ? "" : path_str.substr(0, pos + 1);
}
#endif

// Embedded network weights using generated hex dump header
alignas(32) static const uint8_t raw_nnue_data[] = {
#include "nnue_data.h"
};

// Global NNUE evaluator instance
NNUEEvaluator g_nnue;

NNUEEvaluator::NNUEEvaluator() {
    // Zero-initialize weights and biases
    for (int i = 0; i < 768; ++i) {
        for (int j = 0; j < L1_SIZE; ++j) {
            layer1_weights[i][j] = 0;
        }
    }
    for (int i = 0; i < L1_SIZE; ++i) {
        layer1_biases[i] = 0;
    }
    for (int i = 0; i < 2 * L1_SIZE; ++i) {
        layer2_weights[i] = 0;
    }
    layer2_bias = 0;

    // Load the embedded network weights by default
    load_embedded_network();
}

bool NNUEEvaluator::load_network(const std::string& filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) {
        if (filename.find_first_of("\\/") == std::string::npos) {
            std::string alt_path = get_executable_directory() + filename;
            f = fopen(alt_path.c_str(), "rb");
        }
        if (!f) {
            // Fall back to the embedded coco.nnue weights if the default file is missing
            if (filename == "coco.nnue") {
                return true;
            }
            return false;
        }
    }

    // Verify file size matches expected size for L1_SIZE
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    constexpr long expected_size = (768 * L1_SIZE + L1_SIZE + 2 * L1_SIZE) * sizeof(int16_t) + sizeof(int32_t);
    if (size != expected_size) {
        fclose(f);
        return false;
    }

    // Temporary buffer to hold the raw w1 weights of shape [L1_SIZE][768]
    auto temp_w1 = std::make_unique<int16_t[]>(L1_SIZE * 768);

    // Read layer 1 weights
    if (fread(temp_w1.get(), sizeof(int16_t), L1_SIZE * 768, f) != L1_SIZE * 768) {
        fclose(f);
        return false;
    }

    // Transpose layer 1 weights from [L1_SIZE][768] to [768][L1_SIZE] for cache-contiguous lookups
    for (int i = 0; i < L1_SIZE; ++i) {
        for (int j = 0; j < 768; ++j) {
            layer1_weights[j][i] = temp_w1[i * 768 + j];
        }
    }

    // Read layer 1 biases
    if (fread(layer1_biases, sizeof(int16_t), L1_SIZE, f) != L1_SIZE) {
        fclose(f);
        return false;
    }

    // Read layer 2 weights
    if (fread(layer2_weights, sizeof(int16_t), 2 * L1_SIZE, f) != 2 * L1_SIZE) {
        fclose(f);
        return false;
    }

    // Read layer 2 bias
    if (fread(&layer2_bias, sizeof(int32_t), 1, f) != 1) {
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

void NNUEEvaluator::load_embedded_network() {
    constexpr long expected_size = (768 * L1_SIZE + L1_SIZE + 2 * L1_SIZE) * sizeof(int16_t) + sizeof(int32_t);
    if (sizeof(raw_nnue_data) != expected_size) {
        // Warning: only load if size matches (e.g. if default 256 net is embedded but compiled with L1_SIZE=512)
        std::cerr << "Warning: Embedded NNUE network size (" << sizeof(raw_nnue_data) 
                  << " bytes) does not match expected size for L1_SIZE=" << L1_SIZE 
                  << " (" << expected_size << " bytes). Embedded loading disabled." << std::endl;
        return;
    }

    const uint8_t* ptr = raw_nnue_data;

    // 1. Layer 1 Weights: shape [L1_SIZE][768] in raw file
    const int16_t* temp_w1 = reinterpret_cast<const int16_t*>(ptr);
    ptr += L1_SIZE * 768 * sizeof(int16_t);

    // Transpose layer 1 weights from [L1_SIZE][768] to [768][L1_SIZE]
    for (int i = 0; i < L1_SIZE; ++i) {
        for (int j = 0; j < 768; ++j) {
            layer1_weights[j][i] = temp_w1[i * 768 + j];
        }
    }

    // 2. Layer 1 Biases: L1_SIZE * int16_t
    const int16_t* biases1 = reinterpret_cast<const int16_t*>(ptr);
    ptr += L1_SIZE * sizeof(int16_t);
    for (int i = 0; i < L1_SIZE; ++i) {
        layer1_biases[i] = biases1[i];
    }

    // 3. Layer 2 Weights: 2 * L1_SIZE * int16_t
    const int16_t* weights2 = reinterpret_cast<const int16_t*>(ptr);
    ptr += 2 * L1_SIZE * sizeof(int16_t);
    for (int i = 0; i < 2 * L1_SIZE; ++i) {
        layer2_weights[i] = weights2[i];
    }

    // 4. Layer 2 Bias: 1 * int32_t
    const int32_t* bias2 = reinterpret_cast<const int32_t*>(ptr);
    layer2_bias = *bias2;
}

void NNUEEvaluator::init_accumulator(const Board& board, Accumulator& acc) const {
    // Set all elements of both accumulators to layer 1 biases in aligned chunks
#if defined(__AVX2__)
    for (int i = 0; i < L1_SIZE; i += 16) {
        __m256i bias = _mm256_load_si256((const __m256i*)&layer1_biases[i]);
        _mm256_store_si256((__m256i*)&acc.v[WHITE][i], bias);
        _mm256_store_si256((__m256i*)&acc.v[BLACK][i], bias);
    }
#elif defined(__ARM_NEON)
    for (int i = 0; i < L1_SIZE; i += 8) {
        int16x8_t bias = vld1q_s16(&layer1_biases[i]);
        vst1q_s16(&acc.v[WHITE][i], bias);
        vst1q_s16(&acc.v[BLACK][i], bias);
    }
#else
    for (int i = 0; i < L1_SIZE; ++i) {
        acc.v[WHITE][i] = layer1_biases[i];
        acc.v[BLACK][i] = layer1_biases[i];
    }
#endif

    // Iterate through all 64 squares of the board and activate active features
    for (int sq = 0; sq < 64; ++sq) {
        Piece pc = board.get_piece_at(sq);
        if (pc == NO_PIECE) continue;

        int color = pc / 6;
        int pt = pc % 6;

        int idx_w = get_feature_index_white(color, pt, sq);
        int idx_b = get_feature_index_black(color, pt, sq);

        const int16_t* w_weights = layer1_weights[idx_w];
        const int16_t* b_weights = layer1_weights[idx_b];

#if defined(__AVX2__)
        for (int i = 0; i < L1_SIZE; i += 16) {
            __m256i w_acc = _mm256_load_si256((const __m256i*)&acc.v[WHITE][i]);
            __m256i w_val = _mm256_load_si256((const __m256i*)&w_weights[i]);
            _mm256_store_si256((__m256i*)&acc.v[WHITE][i], _mm256_add_epi16(w_acc, w_val));

            __m256i b_acc = _mm256_load_si256((const __m256i*)&acc.v[BLACK][i]);
            __m256i b_val = _mm256_load_si256((const __m256i*)&b_weights[i]);
            _mm256_store_si256((__m256i*)&acc.v[BLACK][i], _mm256_add_epi16(b_acc, b_val));
        }
#elif defined(__ARM_NEON)
        for (int i = 0; i < L1_SIZE; i += 8) {
            int16x8_t w_acc = vld1q_s16(&acc.v[WHITE][i]);
            int16x8_t w_val = vld1q_s16(&w_weights[i]);
            vst1q_s16(&acc.v[WHITE][i], vaddq_s16(w_acc, w_val));

            int16x8_t b_acc = vld1q_s16(&acc.v[BLACK][i]);
            int16x8_t b_val = vld1q_s16(&b_weights[i]);
            vst1q_s16(&acc.v[BLACK][i], vaddq_s16(b_acc, b_val));
        }
#else
        for (int i = 0; i < L1_SIZE; ++i) {
            acc.v[WHITE][i] += w_weights[i];
            acc.v[BLACK][i] += b_weights[i];
        }
#endif
    }
}

// Helper function to evaluate one perspective of the accumulator using SIMD or C++ fallback.
static inline int32_t evaluate_one_perspective(const int16_t* acc_v, const int16_t* weights) {
#if defined(__AVX2__)
    __m256i sum = _mm256_setzero_si256();
    __m256i zero = _mm256_setzero_si256();
    __m256i max_val = _mm256_set1_epi32(255);
    __m256i magic = _mm256_set1_epi32(32897); // Magic number for division by 255

    for (int i = 0; i < L1_SIZE; i += 16) {
        // Load 16 elements of the accumulator (guaranteed 32-byte aligned)
        __m256i val16 = _mm256_load_si256((const __m256i*)(acc_v + i));

        // Split the 16 elements into two 8-element 32-bit registers
        __m256i val_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(val16));
        __m256i val_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(val16, 1));

        // Clip lo to [0, 255] and compute SCReLU (clamped_x * clamped_x) / 255
        val_lo = _mm256_max_epi32(_mm256_min_epi32(val_lo, max_val), zero);
        __m256i sq_lo = _mm256_mullo_epi32(val_lo, val_lo);
        __m256i div_lo = _mm256_srli_epi32(_mm256_mullo_epi32(sq_lo, magic), 23);

        // Clip hi to [0, 255] and compute SCReLU (clamped_x * clamped_x) / 255
        val_hi = _mm256_max_epi32(_mm256_min_epi32(val_hi, max_val), zero);
        __m256i sq_hi = _mm256_mullo_epi32(val_hi, val_hi);
        __m256i div_hi = _mm256_srli_epi32(_mm256_mullo_epi32(sq_hi, magic), 23);

        // Load 16 elements of layer 2 weights (guaranteed 32-byte aligned)
        __m256i w16 = _mm256_load_si256((const __m256i*)(weights + i));
        __m256i w_lo = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(w16));
        __m256i w_hi = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(w16, 1));

        // Multiply SCReLU activations by weights and sum up
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(div_lo, w_lo));
        sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(div_hi, w_hi));
    }

    // Horizontal sum of the 8 lanes in the sum register (using aligned memory store)
    alignas(32) int32_t temp[8];
    _mm256_store_si256((__m256i*)temp, sum);
    return temp[0] + temp[1] + temp[2] + temp[3] + temp[4] + temp[5] + temp[6] + temp[7];

#elif defined(__ARM_NEON)
    int32x4_t sum = vdupq_n_s32(0);
    int16x8_t zero = vdupq_n_s16(0);
    int16x8_t max_val = vdupq_n_s16(255);
    int32x4_t magic = vdupq_n_s32(32897);

    for (int i = 0; i < L1_SIZE; i += 8) {
        int16x8_t val8 = vld1q_s16(acc_v + i);
        int16x8_t clamped = vmaxq_s16(vminq_s16(val8, max_val), zero);

        int16x4_t clamped_lo = vget_low_s16(clamped);
        int16x4_t clamped_hi = vget_high_s16(clamped);
        int32x4_t sq_lo = vmull_s16(clamped_lo, clamped_lo);
        int32x4_t sq_hi = vmull_s16(clamped_hi, clamped_hi);

        int32x4_t div_lo = vshrq_n_s32(vmulq_s32(sq_lo, magic), 23);
        int32x4_t div_hi = vshrq_n_s32(vmulq_s32(sq_hi, magic), 23);

        int16x8_t w8 = vld1q_s16(weights + i);
        int32x4_t w_lo = vmovl_s16(vget_low_s16(w8));
        int32x4_t w_hi = vmovl_s16(vget_high_s16(w8));

        sum = vaddq_s32(sum, vmulq_s32(div_lo, w_lo));
        sum = vaddq_s32(sum, vmulq_s32(div_hi, w_hi));
    }
    return vgetq_lane_s32(sum, 0) + vgetq_lane_s32(sum, 1) + vgetq_lane_s32(sum, 2) + vgetq_lane_s32(sum, 3);

#else
    int32_t sum = 0;
    for (int i = 0; i < L1_SIZE; ++i) {
        int32_t val = acc_v[i];
        if (val < 0) val = 0;
        else if (val > 255) val = 255;
        int32_t acrelu = (val * val) / 255;
        sum += acrelu * weights[i];
    }
    return sum;
#endif
}

int NNUEEvaluator::evaluate_nnue(const Board& board) const {
    Color us = board.get_side_to_move();
    Color them = (Color)(us ^ 1);
    const Accumulator& acc = board.get_accumulator();

    // Initialize output accumulator with layer 2 bias
    int32_t output_accumulator = layer2_bias;

    // Active accumulator pass (us) uses the first L1_SIZE weights (indices 0..L1_SIZE-1)
    output_accumulator += evaluate_one_perspective(acc.v[us], layer2_weights);

    // Passive accumulator pass (them) uses the second L1_SIZE weights (indices L1_SIZE..2*L1_SIZE-1)
    output_accumulator += evaluate_one_perspective(acc.v[them], layer2_weights + L1_SIZE);

    // Convert scaled quantized probability scalar back into centipawn score
    // Correct scaling factor for SCALE = 400: 400.0 / 16320.0 = 0.0245098
    int score = static_cast<int>(output_accumulator * 0.0245098);

    return score;
}
