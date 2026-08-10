#include "../src/board.h"
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

    Board winning;
    winning.parse_fen("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1");
    tt.clear();
    const int main_fail_high = Search::test_alpha_beta_window(winning, -100, 0, 1);
    require(main_fail_high > 0, "alpha-beta fail-high must retain a soft bound");

    const int q_bound = Search::test_quiescence_window(winning, -100, 0);
    require(q_bound >= 0, "quiescence must respect the fail-high window");

    std::cout << "PASS: fail-soft main cutoff and quiescence bound contract\n";
    return 0;
}
