#ifndef DATAGEN_H
#define DATAGEN_H

#include <string>

// Entry point function to launch multi-threaded self-play harvesting
void run_datagen(long long target_positions, int num_threads, const std::string& output_path);

#endif // DATAGEN_H
