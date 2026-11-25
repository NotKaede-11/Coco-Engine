#include "board.h"
#include "movegen.h"
#include "nnue.h"
#include "uci.h"
#include "search.h"
#include "ravager_eval.h"
#include <cstdio>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "datagen.h"
#include <iostream>
#include <vector>
#include <stdexcept>
#include <filesystem>

namespace {

bool is_interactive_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0;
#endif
}

void print_startup_wordmark() {
    if (!is_interactive_terminal())
        return;

    std::cout
        << "  _____\n"
        << " /  __ \\\n"
        << " | /  \\/ ___   ___ ___\n"
        << " | |    / _ \\ / __/ _ \\\n"
        << " | \\__/\\ (_) | (_| (_) |\n"
        << "  \\____/\\___/ \\___\\___/\n"
        << "\n";
}

} // namespace

int main(int argc, char* argv[]) {
    // Disable I/O buffering for UCI pipe compatibility on Windows.
    // Without this, stdout is fully buffered when connected to a pipe,
    // causing the parent process (e.g., lichess-bot) to never receive responses.
    // NOTE: We intentionally keep sync_with_stdio(true) (the default) so that
    // std::cout shares the same unbuffered FILE* stream set by setbuf.
    std::setbuf(stdout, nullptr);
    std::setbuf(stdin, nullptr);

    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    
    // The fixed release evaluator never depends on a working-directory file.
    // Stale or malformed coco.nnue files cannot replace the selected network.
    if (!RavagerEval::load_embedded()) {
        std::cerr << "Error: Could not initialize Coco's embedded network." << std::endl;
        return 1;
    }

    // Keep UCI stdout pristine for GUIs and match runners, while showing the
    // Coco wordmark when a person launches the engine directly in a terminal.
    print_startup_wordmark();
    
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
                    else if (args[j] == "--game-records" && j + 1 < args.size())
                        options.game_records_path = args[++j];
                    else if (args[j] == "--datagen-depth" && j + 1 < args.size())
                        options.search_depth = std::stoi(args[++j]);
                    else if (args[j] == "--max-game-ply" && j + 1 < args.size())
                        options.max_game_ply = std::stoi(args[++j]);
                    else if (args[j] == "--max-empty-games" && j + 1 < args.size())
                        options.max_empty_games = std::stoi(args[++j]);
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
    
    uci_loop();
    
    return 0;
}

