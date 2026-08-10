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

Search::NmpTestResult probe(const char* fen, Search::NodeType type = Search::NodeType::NON_PV) {
    Board board;
    board.parse_fen(fen);
    tt.clear();
    return Search::test_nmp_window(board, -200, -100, 5, type);
}

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    tt.resize(16);
    if (!g_nnue.load_network("coco.nnue")) return 1;

    const auto queen = probe("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1");
    require(queen.attempts > 0, "NMP must trigger with a safe heavy-piece material edge");

    const auto pawns = probe("4k3/8/8/8/3P4/8/8/4K3 w - - 0 1");
    require(pawns.attempts == 0, "NMP must stay disabled in pawn-only endings");

    const auto lone_minor = probe("4k3/8/8/8/3N4/8/8/4K3 w - - 0 1");
    require(lone_minor.attempts == 0, "NMP must stay disabled with only one minor");

    const auto pv = probe("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1", Search::NodeType::PV);
    require(pv.attempts == 0, "NMP must stay disabled at PV nodes");

    const auto checked = probe("4k3/8/8/8/8/8/4r3/4K3 w - - 0 1");
    require(checked.attempts == 0, "NMP must stay disabled while in check");

    std::cout << "PASS: NMP trigger, PV/check guards, and zugzwang material guards\n";
    return 0;
}
