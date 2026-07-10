#include "uci.h"
#include "types.h"
#include "board.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "evaluate.h"
#include "nnue.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <memory>
#include <vector>

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
void start_search(Board &board, int max_depth, int wtime, int btime, int winc, int binc, int movetime)
{
    stop_search();

    Search::b_abort.store(false, std::memory_order_relaxed);
    Search::start_time = get_time_ms();

    // Compute time controls from clock parameters BEFORE thread launch
    Search::compute_time_controls(board.get_side_to_move(), max_depth, wtime, btime, winc, binc, movetime);

    // Reset per-thread stats
    for (int i = 0; i < Search::num_threads; i++) {
        Search::thread_stats[i].nodes = 0;
        Search::thread_stats[i].seldepth = 0;
    }

    // Spawn Thread 0 (main thread) using heap-allocated board copy
    auto main_board = std::make_unique<Board>(board);
    worker_threads.emplace_back([mb = std::move(main_board), max_depth]() {
        Search::search_position(*mb, max_depth);
        Search::b_abort.store(true, std::memory_order_relaxed);
    });

    // Spawn Helper Threads (1..N-1) using heap-allocated board copies
    for (int i = 1; i < Search::num_threads; i++) {
        auto helper_board = std::make_unique<Board>(board);
        worker_threads.emplace_back([hb = std::move(helper_board), max_depth, i]() {
            Search::search_helper(*hb, max_depth, i);
        });
    }
}

// Parse PGN/UCI move string
Move parse_move_string(const std::string &move_str, const Board &board)
{
    MoveList list;
    generate_pseudo_legal_moves(board, list);

    if (move_str.length() < 4)
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
void parse_position(const std::string &input, Board &board)
{
    std::istringstream iss(input);
    std::string token;
    iss >> token; // Skip "position"
    iss >> token;

    std::string fen;
    if (token == "startpos")
    {
        fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
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
        iss >> token; // Check if there is "moves"
    }
    else
    {
        return;
    }

    board.parse_fen(fen);

    if (token == "moves")
    {
        std::string move_str;
        while (iss >> move_str)
        {
            Move move = parse_move_string(move_str, board);
            if (move.is_none())
            {
                break;
            }
            board.make_move(move);
        }
    }
}

// UCI Go Command Parser
void parse_go(const std::string &input, Board &board)
{
    std::istringstream iss(input);
    std::string token;
    iss >> token; // Skip "go"

    int depth = 64; // Max search depth default
    int wtime = -1;
    int btime = -1;
    int winc = 0;
    int binc = 0;
    int movetime = -1;
    bool infinite = false;

    while (iss >> token)
    {
        if (token == "depth")
        {
            iss >> depth;
        }
        else if (token == "wtime")
        {
            iss >> wtime;
        }
        else if (token == "btime")
        {
            iss >> btime;
        }
        else if (token == "winc")
        {
            iss >> winc;
        }
        else if (token == "binc")
        {
            iss >> binc;
        }
        else if (token == "movetime")
        {
            iss >> movetime;
        }
        else if (token == "infinite")
        {
            infinite = true;
        }
    }

    if (infinite)
    {
        depth = 64;
        wtime = -1;
        movetime = -1;
    }

    start_search(board, depth, wtime, btime, winc, binc, movetime);
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
        Search::start_time = get_time_ms();
        Search::compute_time_controls(b->get_side_to_move(), 10, -1, -1, 0, 0, -1);
        Search::thread_stats[0].nodes = 0;
        Search::thread_stats[0].seldepth = 0;
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
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        if (line == "uci")
        {
            std::cout << "id name Coco v1.3.0\n";
            std::cout << "id author NotKaede-11\n";
            std::cout << "option name Hash type spin default 16 min 1 max 33554432\n";
            std::cout << "option name Threads type spin default 1 min 1 max 1024\n";
            std::cout << "option name RFP_Margin type spin default 75 min 25 max 150\n";
            std::cout << "option name LMR_Constant_Scaled type spin default 225 min 100 max 400\n";
            std::cout << "option name NMP_Base type spin default 3 min 1 max 5\n";
            std::cout << "option name NMP_Divisor type spin default 6 min 3 max 12\n";
            std::cout << "option name Aspiration_Delta type spin default 16 min 4 max 40\n";
            std::cout << "option name History_Threshold type spin default 16384 min 4096 max 32768\n";
            std::cout << "option name Move Overhead type spin default 30 min 0 max 5000\n";
            std::cout << "option name EvalFile type string default coco.nnue\n";
            std::cout << "uciok\n";
        }
        else if (line.rfind("setoption", 0) == 0)
        {
            size_t name_pos = line.find("name ");
            size_t value_pos = line.find("value ");
            if (name_pos != std::string::npos && value_pos != std::string::npos)
            {
                std::string option_name = line.substr(name_pos + 5, value_pos - (name_pos + 5));
                while (!option_name.empty() && isspace(option_name.back()))
                {
                    option_name.pop_back();
                }
                std::string option_value = line.substr(value_pos + 6);
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
                    else if (option_name == "Threads")
                    {
                        int val = std::stoi(option_value);
                        Search::num_threads = std::max(1, std::min(val, MAX_THREADS));
                    }
                    else if (option_name == "Move Overhead")
                    {
                        Search::Move_Overhead = std::stoi(option_value);
                    }
                    else if (option_name == "EvalFile")
                    {
                        if (!g_nnue.load_network(option_value))
                        {
                            std::cout << "info string Warning: Could not load NNUE weights file '" << option_value << "'.\n";
                        }
                        else
                        {
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
            parse_position(line, board);
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
