#ifndef DATAGEN_H
#define DATAGEN_H

#include <cstddef>
#include <cstdint>
#include <string>

struct DatagenOptions {
    uint64_t seed = 0xC0C0DA7AULL;
    size_t buffer_positions = 5000;
    size_t tt_mb_per_worker = 16;
    int max_game_ply = 250;
    std::string opening_book_path;
    std::string engine_path;
    std::string manifest_path;
};

// Entry point function to launch multi-threaded self-play harvesting
bool run_datagen(long long target_positions, int num_threads,
                 const std::string& output_path,
                 const DatagenOptions& options = {});

// The 32-byte Bullet record retains a hard WDL result for format
// compatibility. Trainers can blend that result with the stored evaluation.
double datagen_soft_wdl_target(int16_t score, uint8_t result,
                               double lambda = 0.5, double scale = 400.0);

bool validate_datagen_file(const std::string& path, std::string* error = nullptr);

#endif // DATAGEN_H
