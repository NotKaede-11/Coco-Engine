#include "board.h"
#include "movegen.h"
#include "nnue.h"
#include "uci.h"
#include "search.h"
#include <cstdio>

#include "datagen.h"
#include <iostream>
#include <vector>
#include <stdexcept>
#include <filesystem>

int main(int argc, char* argv[]) {
    // Disable I/O buffering for UCI pipe compatibility on Windows.
    // Without this, stdout is fully buffered when connected to a pipe,
    // causing the parent process (e.g., lichess-bot) to never receive responses.
    // NOTE: We intentionally keep sync_with_stdio(true) (the default) so that
    // std::cout shares the same unbuffered FILE* stream set by setbuf.
    std::setbuf(stdout, nullptr);
    std::setbuf(stdin, nullptr);

    // 1. Initialize engine systems
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    
    // Load NNUE weights from file
    if (!g_nnue.load_network("coco.nnue")) {
        std::cerr << "Error: Could not load NNUE weights file 'coco.nnue'." << std::endl;
        std::cerr << "Please ensure 'coco.nnue' is located in the working directory or next to the executable." << std::endl;
        return 1;
    }
    
    // Parse arguments safely
    std::vector<std::string> args(argv, argv + argc);
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--datagen" && i + 3 < args.size()) {
            try {
                const long long target_positions = std::stoll(args[i + 1]);
                const int num_threads = std::stoi(args[i + 2]);
                const std::string output_path = args[i + 3];
                DatagenOptions options;
                options.engine_path = std::filesystem::absolute(args[0]).string();
                for (size_t j = i + 4; j < args.size(); ++j) {
                    if (args[j] == "--seed" && j + 1 < args.size())
                        options.seed = std::stoull(args[++j]);
                    else if (args[j] == "--book" && j + 1 < args.size())
                        options.opening_book_path = args[++j];
                    else if (args[j] == "--buffer" && j + 1 < args.size())
                        options.buffer_positions = std::stoull(args[++j]);
                    else if (args[j] == "--datagen-tt" && j + 1 < args.size())
                        options.tt_mb_per_worker = std::stoull(args[++j]);
                    else if (args[j] == "--manifest" && j + 1 < args.size())
                        options.manifest_path = args[++j];
                    else
                        throw std::invalid_argument("unknown or incomplete datagen option: " + args[j]);
                }

                std::cout << "[Coco Engine] Initializing Native C++ Data Generator Subsystem..." << std::endl;
                return run_datagen(target_positions, num_threads, output_path, options) ? 0 : 1;
            } catch (const std::exception& error) {
                std::cerr << "Invalid datagen arguments: " << error.what() << '\n';
                return 1;
            }
        }
    }
    
    // 2. Start the UCI protocol listener loop (master loop)
    uci_loop();
    
    return 0;
}

