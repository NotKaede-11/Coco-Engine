#include "nnue.h"
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

// Global NNUE evaluator instance
NNUEEvaluator g_nnue;

NNUEEvaluator::NNUEEvaluator() {
    // Zero-initialize weights and biases
    for (int i = 0; i < 768; ++i) {
        for (int j = 0; j < 256; ++j) {
            layer1_weights[i][j] = 0;
        }
    }
    for (int i = 0; i < 256; ++i) {
        layer1_biases[i] = 0;
    }
    for (int i = 0; i < 512; ++i) {
        layer2_weights[i] = 0;
    }
    layer2_bias = 0;
}

bool NNUEEvaluator::load_network(const std::string& filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f) {
        if (filename.find_first_of("\\/") == std::string::npos) {
            std::string alt_path = get_executable_directory() + filename;
            f = fopen(alt_path.c_str(), "rb");
        }
        if (!f) {
            return false;
        }
    }

    // Verify file size is exactly 394,244 bytes
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size != 394756) {
        fclose(f);
        return false;
    }

    // Temporary buffer to hold the raw w1 weights of shape [256][768]
    auto temp_w1 = std::make_unique<int16_t[]>(256 * 768);

    // Read layer 1 weights
    if (fread(temp_w1.get(), sizeof(int16_t), 256 * 768, f) != 256 * 768) {
        fclose(f);
        return false;
    }

    // Transpose layer 1 weights from [256][768] to [768][256] for cache-contiguous lookups
    for (int i = 0; i < 256; ++i) {
        for (int j = 0; j < 768; ++j) {
            layer1_weights[j][i] = temp_w1[i * 768 + j];
        }
    }

    // Read layer 1 biases
    if (fread(layer1_biases, sizeof(int16_t), 256, f) != 256) {
        fclose(f);
        return false;
    }

    // Read layer 2 weights
    if (fread(layer2_weights, sizeof(int16_t), 512, f) != 512) {
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

void NNUEEvaluator::init_accumulator(const Board& board, Accumulator& acc) const {
    // Set all elements of both accumulators to layer 1 biases
    for (int side = 0; side < 2; ++side) {
        for (int i = 0; i < 256; ++i) {
            acc.v[side][i] = layer1_biases[i];
        }
    }

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

        for (int i = 0; i < 256; ++i) {
            acc.v[WHITE][i] += w_weights[i];
            acc.v[BLACK][i] += b_weights[i];
        }
    }
}

int NNUEEvaluator::evaluate_nnue(const Board& board) const {
    Color us = board.get_side_to_move();
    Color them = (Color)(us ^ 1);
    const Accumulator& acc = board.get_accumulator();

    // Initialize output accumulator with layer 2 bias
    int32_t output_accumulator = layer2_bias;

    // Active accumulator pass (us) uses the second 256 weights (indices 256..511)
    for (int i = 0; i < 256; ++i) {
        int32_t val = acc.v[us][i];
        int32_t activated = std::clamp((int32_t)val, (int32_t)0, (int32_t)255);
        int32_t squared = (activated * activated) / 255;
        output_accumulator += squared * layer2_weights[256 + i];
    }

    // Passive accumulator pass (them) uses the first 256 weights (indices 0..255)
    for (int i = 0; i < 256; ++i) {
        int32_t val = acc.v[them][i];
        int32_t activated = std::clamp((int32_t)val, (int32_t)0, (int32_t)255);
        int32_t squared = (activated * activated) / 255;
        output_accumulator += squared * layer2_weights[i];
    }

    // Convert scaled quantized probability scalar back into centipawn score
    // Correct scaling factor for SCALE = 400: 400.0 / 16320.0 = 0.0245098
    int score = static_cast<int>(output_accumulator * 0.0245098);

    return score;
}
