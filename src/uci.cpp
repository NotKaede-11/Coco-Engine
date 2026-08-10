#include "uci.h"
#include "types.h"
#include "board.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "evaluate.h"
#include "nnue.h"
#include "tbprobe.h"
#include "build_info.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>
#include <algorithm>
#include <charconv>
#include <type_traits>

static std::string compiler_identity() {
#if defined(__clang__)
    return "clang-" + std::to_string(__clang_major__) + "."
        + std::to_string(__clang_minor__) + "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "gcc-" + std::to_string(__GNUC__) + "."
        + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "msvc-" + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

static std::string compiled_isa() {
    std::string result;
    auto add = [&](const char* feature) {
        if (!result.empty()) result += "+";
        result += feature;
    };
#if defined(__aarch64__) || defined(_M_ARM64)
    add("arm64");
#if defined(__ARM_FEATURE_DOTPROD)
    add("dotprod");
#endif
#else
    add("x86-64");
#if defined(__POPCNT__)
    add("popcnt");
#endif
#if defined(__SSE4_1__)
    add("sse4.1");
#endif
#if defined(__AVX2__)
    add("avx2");
#endif
#if defined(__BMI2__)
    add("bmi2");
#endif
#if defined(__AVX512F__)
    add("avx512f");
#endif
#endif
    return result;
}

// Worker thread pool for background search
std::vector<std::thread> worker_threads;

// Recursive Perft function
uint64_t perft(int depth, Board &board)
{
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);

    if (depth == 1)
    {
        uint64_t nodes = 0;
        for (int i = 0; i < move_list.count; i++)
        {
            if (board.make_move(move_list.moves[i]))
            {
                nodes++;
                board.unmake_move(move_list.moves[i]);
            }
        }
        return nodes;
    }

    if (depth == 0)
        return 1ULL;

    uint64_t nodes = 0;
    for (int i = 0; i < move_list.count; i++)
    {
        if (!board.make_move(move_list.moves[i]))
        {
            continue; // Skip illegal moves
        }
        nodes += perft(depth - 1, board);
        board.unmake_move(move_list.moves[i]);
    }
    return nodes;
}

// Perft divide for troubleshooting
void run_perft_divide(int depth, Board &board)
{
    auto start = std::chrono::high_resolution_clock::now();
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    uint64_t total_nodes = 0;

    std::cout << "\n";
    for (int i = 0; i < move_list.count; i++)
    {
        Move m = move_list.moves[i];
        if (!board.make_move(m))
            continue;

        std::string m_str = square_to_str(m.from()) + square_to_str(m.to());
        if (m.is_promotion())
        {
            int pt = m.promotion_piece_type();
            if (pt == KNIGHT)
                m_str += "n";
            else if (pt == BISHOP)
                m_str += "b";
            else if (pt == ROOK)
                m_str += "r";
            else if (pt == QUEEN)
                m_str += "q";
        }

        uint64_t nodes = (depth > 1) ? perft(depth - 1, board) : 1ULL;
        board.unmake_move(m);
        total_nodes += nodes;

        std::cout << m_str << ": " << nodes << "\n";
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "\nTotal nodes: " << total_nodes << "\n";
    std::cout << "Time taken: " << ms << " ms\n";
    if (ms > 0)
    {
        std::cout << "NPS: " << (total_nodes * 1000) / ms << "\n";
    }
    std::cout << "\n";
}

static inline uint64_t get_time_ms() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

// Stop the background search
void stop_search()
{
    Search::b_abort.store(true, std::memory_order_relaxed);
    Search::pondering.store(false, std::memory_order_relaxed);
    for (auto &t : worker_threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }
    worker_threads.clear();
}

// Start search in background thread pool (Lazy SMP)
void start_search(Board &board, const Search::Limits &limits)
{
    stop_search();

    Search::b_abort.store(false, std::memory_order_relaxed);
    Search::pondering.store(limits.ponder && Search::Ponder, std::memory_order_relaxed);
    // Pre-register every helper so the main thread can safely wait even when
    // it finishes before an operating-system worker has begun executing.
    Search::active_helpers.store(Search::num_threads - 1, std::memory_order_relaxed);
    Search::start_time.store(get_time_ms(), std::memory_order_relaxed);

    // Compute time controls from clock parameters BEFORE thread launch
    Search::compute_time_controls(board.get_side_to_move(), limits);
    Search::set_root_moves(limits.searchmoves, limits.searchmoves_specified);
    Search::reset_root_node_accounting(board);

    // Reset per-thread stats
    for (int i = 0; i < Search::num_threads; i++) {
        Search::thread_stats[i].nodes.store(0, std::memory_order_relaxed);
        Search::thread_stats[i].tbhits.store(0, std::memory_order_relaxed);
        Search::thread_stats[i].seldepth.store(0, std::memory_order_relaxed);
    }

    // Preserve the accepted main-first Lazy SMP launch order.
    auto main_board = std::make_unique<Board>(board);
    worker_threads.emplace_back([mb = std::move(main_board), max_depth = limits.depth]() {
        Search::search_position(*mb, max_depth);
        Search::b_abort.store(true, std::memory_order_release);
    });

    for (int i = 1; i < Search::num_threads; i++) {
        auto helper_board = std::make_unique<Board>(board);
        worker_threads.emplace_back([hb = std::move(helper_board), max_depth = limits.depth, i]() {
            Search::search_helper(*hb, max_depth, i);
        });
    }
}

// Parse PGN/UCI move string
Move parse_move_string(const std::string &move_str, const Board &board)
{
    MoveList list;
    generate_pseudo_legal_moves(board, list);

    if ((move_str.length() != 4 && move_str.length() != 5)
        || move_str[0] < 'a' || move_str[0] > 'h'
        || move_str[1] < '1' || move_str[1] > '8'
        || move_str[2] < 'a' || move_str[2] > 'h'
        || move_str[3] < '1' || move_str[3] > '8')
        return Move();

    int from = (move_str[0] - 'a') + (move_str[1] - '1') * 8;
    int to = (move_str[2] - 'a') + (move_str[3] - '1') * 8;

    char promo_char = (move_str.length() >= 5) ? tolower(move_str[4]) : '\0';

    for (int i = 0; i < list.count; i++)
    {
        Move m = list.moves[i];
        if (m.from() == from && m.to() == to)
        {
            if (m.is_promotion())
            {
                int p_type = m.promotion_piece_type();
                char p_char = '\0';
                if (p_type == KNIGHT)
                    p_char = 'n';
                else if (p_type == BISHOP)
                    p_char = 'b';
                else if (p_type == ROOK)
                    p_char = 'r';
                else if (p_type == QUEEN)
                    p_char = 'q';

                if (promo_char == p_char)
                    return m;
            }
            else
            {
                if (promo_char == '\0')
                    return m;
            }
        }
    }
    return Move();
}

// UCI Position Command Parser
bool parse_position(const std::string &input, Board &board)
{
    std::istringstream iss(input);
    std::string token;
    iss >> token; // Skip "position"
    iss >> token;

    std::string fen;
    if (token == "startpos")
    {
        fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        token.clear();
        if (iss >> token && token == "moves")
        {
            // Found "moves" token
        }
    }
    else if (token == "fen")
    {
        std::string f1, f2, f3, f4, f5, f6;
        iss >> f1 >> f2 >> f3 >> f4 >> f5 >> f6;
        fen = f1 + " " + f2 + " " + f3 + " " + f4 + " " + f5 + " " + f6;
        token.clear();
        iss >> token; // Check if there is "moves"
    }
    else
    {
        return false;
    }

    Board candidate;
    if (!candidate.parse_fen(fen))
        return false;

    if (token == "moves")
    {
        std::string move_str;
        while (iss >> move_str)
        {
            Move move = parse_move_string(move_str, candidate);
            if (move.is_none())
            {
                return false;
            }
            if (!candidate.make_move(move))
                return false;
        }
    }
    else if (!token.empty())
    {
        return false;
    }

    board = candidate;
    return true;
}

// UCI Go Command Parser
void parse_go(const std::string &input, Board &board)
{
    std::istringstream iss(input);
    std::string token;
    iss >> token; // Skip "go"
    std::vector<std::string> tokens;
    while (iss >> token) tokens.push_back(token);

    Search::Limits limits;

    auto is_keyword = [](const std::string& value) {
        return value == "depth" || value == "wtime" || value == "btime"
            || value == "winc" || value == "binc" || value == "movetime"
            || value == "movestogo" || value == "nodes" || value == "mate"
            || value == "infinite" || value == "ponder" || value == "searchmoves";
    };
    auto parse_integer = [](const std::string& text, auto& value) {
        using Value = std::remove_reference_t<decltype(value)>;
        Value parsed{};
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
        value = parsed;
        return true;
    };

    for (size_t i = 0; i < tokens.size(); ++i) {
        token = tokens[i];
        auto next_number = [&](auto& destination) {
            if (i + 1 < tokens.size() && parse_integer(tokens[i + 1], destination)) {
                ++i;
                return true;
            }
            return false;
        };
        if (token == "depth") next_number(limits.depth);
        else if (token == "wtime") next_number(limits.wtime);
        else if (token == "btime") next_number(limits.btime);
        else if (token == "winc") next_number(limits.winc);
        else if (token == "binc") next_number(limits.binc);
        else if (token == "movetime") next_number(limits.movetime);
        else if (token == "movestogo") next_number(limits.movestogo);
        else if (token == "nodes") next_number(limits.nodes);
        else if (token == "mate") next_number(limits.mate);
        else if (token == "infinite") limits.infinite = true;
        else if (token == "ponder") limits.ponder = true;
        else if (token == "searchmoves") {
            limits.searchmoves_specified = true;
            const LegalityMasks masks = board.get_legality_masks();
            while (i + 1 < tokens.size() && !is_keyword(tokens[i + 1])) {
                const std::string move_token = tokens[++i];
                const Move move = parse_move_string(move_token, board);
                const bool legal = !move.is_none() && board.is_move_legal(move, masks);
                if (!legal) {
                    std::cout << "info string rejected illegal searchmoves token "
                              << move_token << "\n";
                    continue;
                }
                if (std::find(limits.searchmoves.begin(), limits.searchmoves.end(), move)
                    == limits.searchmoves.end()) limits.searchmoves.push_back(move);
            }
        }
    }

    limits.depth = std::clamp(limits.depth, 1, 64);
    limits.winc = std::max(0, limits.winc);
    limits.binc = std::max(0, limits.binc);
    limits.movestogo = std::max(0, limits.movestogo);
    limits.mate = std::clamp(limits.mate, 0, 32);
    if (limits.mate > 0)
        limits.depth = std::min(limits.depth, std::min(64, limits.mate * 2));

    start_search(board, limits);
}

// Run a standardized benchmark over 5 positions to depth 10
void run_benchmark()
{
    std::vector<std::string> bench_positions = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",             // Startpos
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", // Kiwipete
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",                            // Position 3
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 2",     // Position 4
        "rnbqkb1r/ppppp1pp/7n/5p2/8/6P1/PPPPPP1P/RNBQKBNR w KQkq - 0 1"         // Position 5
    };

    uint64_t total_nodes = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    std::cout << "\n=== Running Engine Benchmark ===" << std::endl;

    for (size_t i = 0; i < bench_positions.size(); ++i)
    {
        auto b = std::make_unique<Board>();
        b->parse_fen(bench_positions[i]);

        // Clear transposition table to ensure determinism
        tt.clear();

        std::cout << "Benchmarking position " << (i + 1) << "..." << std::endl;

        // Run search synchronously to depth 10
        Search::b_abort.store(false, std::memory_order_relaxed);
        Search::start_time.store(get_time_ms(), std::memory_order_relaxed);
        Search::Limits limits;
        limits.depth = 10;
        Search::compute_time_controls(b->get_side_to_move(), limits);
        Search::set_root_moves({}, false);
        Search::reset_root_node_accounting(*b);
        Search::active_helpers.store(0, std::memory_order_relaxed);
        Search::thread_stats[0].nodes.store(0, std::memory_order_relaxed);
        Search::thread_stats[0].tbhits.store(0, std::memory_order_relaxed);
        Search::thread_stats[0].seldepth.store(0, std::memory_order_relaxed);
        Search::search_position(*b, 10);

        // Read nodes_visited
        extern thread_local uint64_t nodes_visited;
        total_nodes += nodes_visited;
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "\n===============================" << std::endl;
    std::cout << "Total nodes searched: " << total_nodes << std::endl;
    std::cout << "Time elapsed: " << elapsed_ms << " ms" << std::endl;
    if (elapsed_ms > 0)
    {
        std::cout << "Nodes Per Second (NPS): " << (total_nodes * 1000) / elapsed_ms << std::endl;
    }
    std::cout << "===============================" << std::endl;
}

// Master UCI lifecycle loop
void uci_loop()
{
    auto board_ptr = std::make_unique<Board>();
    Board &board = *board_ptr;
    board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    std::string line;
    bool debug_mode = false;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        if (line == "debug on" || line == "debug off")
        {
            debug_mode = (line == "debug on");
            std::cout << "info string Debug mode " << (debug_mode ? "on" : "off") << "\n";
        }
        else if (line == "uci")
        {
            std::cout << "id name Coco pre-release\n";
            std::cout << "id author NotKaede-11\n";
            std::cout << "info string build arch=" << COCO_BUILD_ARCH
                      << " isa=" << compiled_isa()
                      << " compiler=" << compiler_identity()
                      << " requested_compiler=" << COCO_BUILD_REQUESTED_COMPILER
                      << " embedded_nnue_sha256=" << COCO_EMBEDDED_NNUE_SHA256
                      << " embedded_nnue_bytes=" << COCO_EMBEDDED_NNUE_SIZE
                      << " active_nnue=" << g_nnue.network_fingerprint()
                      << " active_source=" << g_nnue.network_source() << "\n";
            std::cout << "option name Hash type spin default 16 min 1 max 33554432\n";
            std::cout << "option name Clear Hash type button\n";
            std::cout << "option name Threads type spin default 1 min 1 max 1024\n";
            std::cout << "option name Ponder type check default false\n";
            std::cout << "option name MultiPV type spin default 1 min 1 max 256\n";
            std::cout << "option name Use PEXT type check default "
                      << (pext_available() ? "true" : "false") << "\n";
            std::cout << "option name RFP_Margin type spin default 70 min 25 max 150\n";
            std::cout << "option name LMR_Constant_Scaled type spin default 218 min 100 max 400\n";
            std::cout << "option name NMP_Base type spin default 3 min 1 max 5\n";
            std::cout << "option name NMP_Divisor type spin default 7 min 3 max 12\n";
            std::cout << "option name Aspiration_Delta type spin default 18 min 4 max 40\n";
            std::cout << "option name History_Threshold type spin default 15576 min 4096 max 32768\n";
            std::cout << "option name Move Overhead type spin default 30 min 0 max 5000\n";
            std::cout << "option name EvalFile type string default coco.nnue\n";
            std::cout << "option name SyzygyPath type string default <empty>\n";
            std::cout << "option name SyzygyProbeDepth type spin default 1 min 1 max 100\n";
            std::cout << "option name SyzygyProbeLimit type check default true\n";
            std::cout << "option name Syzygy50MoveRule type check default true\n";
            std::cout << "option name UCI_ShowWDL type check default false\n";
            std::cout << "option name UCI_AnalyseMode type check default false\n";
            std::cout << "option name LMR_History_Divisor type spin default 7302 min 1024 max 32768\n";
            std::cout << "option name Contempt type spin default 0 min -100 max 100\n";
            std::cout << "option name SEE_Pruning_Depth type spin default 0 min 0 max 20\n";
            std::cout << "uciok\n";
        }
        else if (line.rfind("setoption", 0) == 0)
        {
            stop_search();
            size_t name_pos = line.find("name ");
            size_t value_pos = line.find("value ");
            if (name_pos != std::string::npos)
            {
                size_t name_end = value_pos == std::string::npos ? line.size() : value_pos;
                std::string option_name = line.substr(name_pos + 5, name_end - (name_pos + 5));
                while (!option_name.empty() && isspace(option_name.back()))
                {
                    option_name.pop_back();
                }
                std::string option_value = value_pos == std::string::npos
                    ? std::string()
                    : line.substr(value_pos + 6);
                while (!option_value.empty() && isspace(option_value.back()))
                {
                    option_value.pop_back();
                }

                try
                {
                    if (option_name == "Hash")
                    {
                        tt.resize(std::stoi(option_value));
                    }
                    else if (option_name == "Clear Hash")
                    {
                        tt.clear();
                    }
                    else if (option_name == "Threads")
                    {
                        int val = std::stoi(option_value);
                        Search::num_threads = std::max(1, std::min(val, MAX_THREADS));
                    }
                    else if (option_name == "Ponder")
                    {
                        Search::Ponder = (option_value == "true");
                    }
                    else if (option_name == "MultiPV")
                    {
                        Search::MultiPV = std::clamp(std::stoi(option_value), 1, 256);
                    }
                    else if (option_name == "Use PEXT")
                    {
                        set_pext_enabled(option_value == "true");
                    }
                    else if (option_name == "Move Overhead")
                    {
                        Search::Move_Overhead = std::stoi(option_value);
                    }
                    else if (option_name == "EvalFile")
                    {
                        stop_search();
                        std::string current_fen = board.get_fen();
                        if (!g_nnue.load_network(option_value))
                        {
                            std::cout << "info string Warning: Could not load NNUE weights file '" << option_value << "'.\n";
                        }
                        else
                        {
                            // Accumulators are network-specific. Preserve the
                            // position while rebuilding it under the new net.
                            board.parse_fen(current_fen);
                            std::cout << "info string NNUE weights file loaded successfully: '" << option_value << "'.\n";
                        }
                    }
                    else if (option_name == "RFP_Margin")
                    {
                        Search::RFP_Margin = std::stoi(option_value);
                    }
                    else if (option_name == "LMR_Constant_Scaled")
                    {
                        Search::LMR_Constant_Scaled = std::stoi(option_value);
                        Search::init_search_tables();
                    }
                    else if (option_name == "NMP_Base")
                    {
                        Search::NMP_Base = std::stoi(option_value);
                    }
                    else if (option_name == "NMP_Divisor")
                    {
                        Search::NMP_Divisor = std::stoi(option_value);
                    }
                    else if (option_name == "Aspiration_Delta")
                    {
                        Search::Aspiration_Delta = std::stoi(option_value);
                    }
                    else if (option_name == "History_Threshold")
                    {
                        Search::History_Threshold = std::stoi(option_value);
                    }
                    else if (option_name == "SyzygyPath")
                    {
                        if (option_value == "<empty>" || option_value.empty()) {
                            tb_free();
                        } else {
                            tb_init(option_value.c_str());
                        }
                    }
                    else if (option_name == "SyzygyProbeDepth")
                    {
                        Search::SyzygyProbeDepth = std::stoi(option_value);
                    }
                    else if (option_name == "SyzygyProbeLimit")
                    {
                        Search::SyzygyProbeLimit = (option_value == "true");
                    }
                    else if (option_name == "Syzygy50MoveRule")
                    {
                        Search::Syzygy50MoveRule = (option_value == "true");
                    }
                    else if (option_name == "UCI_ShowWDL")
                    {
                        Search::UCI_ShowWDL = (option_value == "true");
                    }
                    else if (option_name == "UCI_AnalyseMode")
                    {
                        Search::UCI_AnalyseMode = (option_value == "true");
                    }
                    else if (option_name == "LMR_History_Divisor")
                    {
                        Search::LMR_History_Divisor = std::stoi(option_value);
                    }
                    else if (option_name == "Contempt")
                    {
                        Search::Contempt = std::stoi(option_value);
                    }
                    else if (option_name == "SEE_Pruning_Depth")
                    {
                        Search::SEE_Pruning_Depth = std::stoi(option_value);
                    }
                }
                catch (...)
                {
                    // Ignore malformed GUI option settings
                }
            }
        }
        else if (line == "isready")
        {
            std::cout << "readyok\n";
        }
        else if (line == "ucinewgame")
        {
            stop_search();
            tt.clear();
        }
        else if (line.rfind("position", 0) == 0)
        {
            stop_search();
            if (!parse_position(line, board))
                std::cout << "info string rejected malformed position command\n";
        }
        else if (line.rfind("go perft", 0) == 0)
        {
            std::istringstream iss(line);
            std::string dummy1, dummy2;
            int depth = 1;
            iss >> dummy1 >> dummy2 >> depth;
            run_perft_divide(depth, board);
        }
        else if (line.rfind("go", 0) == 0)
        {
            parse_go(line, board);
        }
        else if (line == "bench")
        {
            stop_search();
            run_benchmark();
        }
        else if (line == "stop")
        {
            stop_search();
        }
        else if (line == "ponderhit")
        {
            Search::ponder_hit();
        }
        else if (line == "eval")
        {
            std::cout << "Evaluation: " << Evaluation::evaluate(board) << "\n";
        }
        else if (line == "d")
        {
            board.print();
        }
        else if (line == "quit")
        {
            stop_search();
            tb_free();
            break;
        }
    }
}
