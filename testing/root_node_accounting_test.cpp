#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/search.h"
#include "../src/tt.h"
#include <cstdlib>
#include <iostream>

extern thread_local uint64_t nodes_visited;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    tt.resize(16);
    if (!g_nnue.load_network("coco.nnue")) return 1;

    Board board;
    board.parse_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    Search::reset_root_node_accounting(board);
    tt.clear();
    Search::test_alpha_beta_window(board, -INFINITY_SCORE, INFINITY_SCORE, 4);

    int score = 0;
    Move best_move;
    tt.probe(board.get_hash_key(), score, best_move, 4,
             -INFINITY_SCORE, INFINITY_SCORE, 0);

    const uint64_t root_total = Search::test_root_nodes_total();
    require(root_total > 0, "root search must publish node totals");
    require(root_total <= nodes_visited,
            "root-move totals must not exceed the thread's searched nodes");
    require(!best_move.is_none() && Search::test_root_nodes_for(best_move) > 0,
            "the selected root move must own a non-zero node count");

    Search::reset_root_node_accounting(board);
    require(Search::test_root_nodes_total() == 0,
            "a new search must clear all root-move node totals");

    std::cout << "PASS: root-move node totals are bounded, attributed, and reset\n";
    return 0;
}
