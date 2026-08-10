#include "../src/board.h"
#include "../src/evaluate.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/search.h"
#include "../src/tt.h"
#include <cstdlib>
#include <iostream>

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

    Board mate;
    mate.parse_fen("7k/6Q1/6K1/8/8/8/8/8 b - - 0 1");
    tt.clear();
    require(Search::test_quiescence_window(mate, -50000, 50000) <= -29000,
            "qsearch must recognize checkmate instead of standing pat");

    Board quiet_evasion;
    quiet_evasion.parse_fen("7k/8/8/8/8/8/7R/6K1 b - - 0 1");
    const std::string original_fen = quiet_evasion.get_fen();
    const U64 original_hash = quiet_evasion.get_hash_key();
    tt.clear();
    const int evasion_score = Search::test_quiescence_window(quiet_evasion, -50000, 50000);
    require(evasion_score > -29000, "qsearch must search a legal quiet check evasion");
    require(quiet_evasion.get_hash_key() == original_hash && quiet_evasion.get_fen() == original_fen,
            "qsearch must restore the board after evasions");

    Board promotion;
    promotion.parse_fen("k7/4P3/8/8/8/8/8/7K w - - 0 1");
    const int stand_pat = Evaluation::evaluate(promotion);
    tt.clear();
    const int promoted = Search::test_quiescence_window(promotion, -50000, 50000);
    require(promoted > stand_pat + 200, "qsearch must include quiet promotions");

    std::cout << "PASS: qsearch checkmate, quiet evasion, board restore, and quiet promotion\n";
    return 0;
}
