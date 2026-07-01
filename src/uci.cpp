#include "uci.h"
#include "types.h"
#include "board.h"
#include "movegen.h"
#include "search.h"
#include "tt.h"
#include "evaluate.h"
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>

// Worker thread for background search
std::thread worker_thread;

// Recursive Perft function
uint64_t perft(int depth, Board& board) {
    if (depth == 0) return 1ULL;
    
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    uint64_t nodes = 0;
    
    for (int i = 0; i < move_list.count; i++) {
        if (!board.make_move(move_list.moves[i])) {
            continue; // Skip illegal moves
        }
        nodes += perft(depth - 1, board);
        board.unmake_move(move_list.moves[i]);
    }
    return nodes;
}

// Perft divide for troubleshooting
void run_perft_divide(int depth, Board& board) {
    auto start = std::chrono::high_resolution_clock::now();
    MoveList move_list;
    generate_pseudo_legal_moves(board, move_list);
    uint64_t total_nodes = 0;
    
    std::cout << "\n";
    for (int i = 0; i < move_list.count; i++) {
        Move m = move_list.moves[i];
        if (!board.make_move(m)) continue;
        
        std::string m_str = square_to_str(m.from()) + square_to_str(m.to());
        if (m.is_promotion()) {
            int pt = m.promotion_piece_type();
            if (pt == KNIGHT) m_str += "n";
            else if (pt == BISHOP) m_str += "b";
            else if (pt == ROOK) m_str += "r";
            else if (pt == QUEEN) m_str += "q";
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
    if (ms > 0) {
        std::cout << "NPS: " << (total_nodes * 1000) / ms << "\n";
    }
    std::cout << "\n";
}

// Search wrapper function to safely pass board by value to the thread
void search_wrapper(Board board, int max_depth, int wtime, int btime, int winc, int binc, int movetime) {
    Search::search_position(board, max_depth, wtime, btime, winc, binc, movetime);
}

// Start search in the background thread
void start_search(Board& board, int max_depth, int wtime, int btime, int winc, int binc, int movetime) {
    // If a thread is already running, abort it and wait for it to join
    if (worker_thread.joinable()) {
        Search::b_abort = true;
        worker_thread.join();
    }
    Search::b_abort = false;
    worker_thread = std::thread(search_wrapper, board, max_depth, wtime, btime, winc, binc, movetime);
}

// Stop the background search
void stop_search() {
    if (worker_thread.joinable()) {
        Search::b_abort = true;
        worker_thread.join();
    }
}

// Parse PGN/UCI move string
Move parse_move_string(const std::string& move_str, const Board& board) {
    MoveList list;
    generate_pseudo_legal_moves(board, list);
    
    if (move_str.length() < 4) return Move();
    
    int from = (move_str[0] - 'a') + (move_str[1] - '1') * 8;
    int to = (move_str[2] - 'a') + (move_str[3] - '1') * 8;
    
    char promo_char = (move_str.length() >= 5) ? tolower(move_str[4]) : '\0';
    
    for (int i = 0; i < list.count; i++) {
        Move m = list.moves[i];
        if (m.from() == from && m.to() == to) {
            if (m.is_promotion()) {
                int p_type = m.promotion_piece_type();
                char p_char = '\0';
                if (p_type == KNIGHT) p_char = 'n';
                else if (p_type == BISHOP) p_char = 'b';
                else if (p_type == ROOK) p_char = 'r';
                else if (p_type == QUEEN) p_char = 'q';
                
                if (promo_char == p_char) return m;
            } else {
                if (promo_char == '\0') return m;
            }
        }
    }
    return Move();
}

// UCI Position Command Parser
void parse_position(const std::string& input, Board& board) {
    std::istringstream iss(input);
    std::string token;
    iss >> token; // Skip "position"
    iss >> token;
    
    std::string fen;
    if (token == "startpos") {
        fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
        if (iss >> token && token == "moves") {
            // Found "moves" token
        }
    } else if (token == "fen") {
        std::string f1, f2, f3, f4, f5, f6;
        iss >> f1 >> f2 >> f3 >> f4 >> f5 >> f6;
        fen = f1 + " " + f2 + " " + f3 + " " + f4 + " " + f5 + " " + f6;
        iss >> token; // Check if there is "moves"
    } else {
        return;
    }
    
    board.parse_fen(fen);
    
    if (token == "moves") {
        std::string move_str;
        while (iss >> move_str) {
            Move move = parse_move_string(move_str, board);
            if (move.is_none()) {
                break;
            }
            board.make_move(move);
        }
    }
}

// UCI Go Command Parser
void parse_go(const std::string& input, Board& board) {
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
    
    while (iss >> token) {
        if (token == "depth") {
            iss >> depth;
        } else if (token == "wtime") {
            iss >> wtime;
        } else if (token == "btime") {
            iss >> btime;
        } else if (token == "winc") {
            iss >> winc;
        } else if (token == "binc") {
            iss >> binc;
        } else if (token == "movetime") {
            iss >> movetime;
        } else if (token == "infinite") {
            infinite = true;
        }
    }
    
    if (infinite) {
        depth = 64;
        wtime = -1;
        movetime = -1;
    }
    
    start_search(board, depth, wtime, btime, winc, binc, movetime);
}

// Master UCI lifecycle loop
void uci_loop() {
    Board board;
    board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        
        if (line == "uci") {
            std::cout << "id name Coco Chess Engine\n";
            std::cout << "id author NotKaede-11\n";
            std::cout << "option name Hash type spin default 16 min 1 max 33554432\n";
            std::cout << "option name Threads type spin default 1 min 1 max 1024\n";
            std::cout << "option name RFP_Margin type spin default 75 min 25 max 150\n";
            std::cout << "option name LMR_Constant_Scaled type spin default 225 min 100 max 400\n";
            std::cout << "option name NMP_Base type spin default 3 min 1 max 5\n";
            std::cout << "option name NMP_Divisor type spin default 6 min 3 max 12\n";
            std::cout << "option name Aspiration_Delta type spin default 16 min 4 max 40\n";
            std::cout << "option name History_Threshold type spin default 16384 min 4096 max 32768\n";
            std::cout << "uciok\n";
        } else if (line.rfind("setoption", 0) == 0) {
            std::istringstream iss(line);
            std::string token, name_label, name, value_label;
            iss >> token >> name_label >> name >> value_label;
            if (name_label == "name" && value_label == "value") {
                int val;
                if (iss >> val) {
                    if (name == "Hash") {
                        tt.resize(val);
                    } else if (name == "RFP_Margin") {
                        Search::RFP_Margin = val;
                    } else if (name == "LMR_Constant_Scaled") {
                        Search::LMR_Constant_Scaled = val;
                        Search::init_search_tables();
                    } else if (name == "NMP_Base") {
                        Search::NMP_Base = val;
                    } else if (name == "NMP_Divisor") {
                        Search::NMP_Divisor = val;
                    } else if (name == "Aspiration_Delta") {
                        Search::Aspiration_Delta = val;
                    } else if (name == "History_Threshold") {
                        Search::History_Threshold = val;
                    }
                }
            }
        } else if (line == "isready") {
            std::cout << "readyok\n";
        } else if (line == "ucinewgame") {
            stop_search();
            tt.clear();
        } else if (line.rfind("position", 0) == 0) {
            stop_search();
            parse_position(line, board);
        } else if (line.rfind("go perft", 0) == 0) {
            std::istringstream iss(line);
            std::string dummy1, dummy2;
            int depth = 1;
            iss >> dummy1 >> dummy2 >> depth;
            run_perft_divide(depth, board);
        } else if (line.rfind("go", 0) == 0) {
            parse_go(line, board);
        } else if (line == "stop") {
            stop_search();
        } else if (line == "eval") {
            std::cout << "Evaluation: " << Evaluation::evaluate(board) << "\n";
        } else if (line == "d") {
            board.print();
        } else if (line == "quit") {
            stop_search();
            break;
        }
    }
}
