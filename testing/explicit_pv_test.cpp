#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/search.h"
#include "../src/tt.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    Search::num_threads = 1;
    tt.resize(16);
    require(g_nnue.load_network("coco.nnue"), "load NNUE");

    Board board;
    require(board.parse_fen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"),
        "parse fixture");
    Search::reset_root_node_accounting(board);
    tt.clear();
    Search::test_alpha_beta_pv(board, -INFINITY_SCORE, INFINITY_SCORE, 5);

    Move before_clear[64]{};
    const int before_length = Search::test_get_explicit_pv(before_clear, 64);
    require(before_length > 1, "explicit PV did not propagate beyond the root");

    tt.clear();
    Move after_clear[64]{};
    const int after_length = Search::test_get_explicit_pv(after_clear, 64);
    require(after_length == before_length,
            "explicit PV length depended on the transposition table");
    for (int ply = 0; ply < after_length; ++ply) {
        require(after_clear[ply] == before_clear[ply],
                "explicit PV changed after clearing the transposition table");
        require(board.make_move(after_clear[ply]),
                "explicit PV contains an illegal move");
    }
    for (int ply = after_length - 1; ply >= 0; --ply)
        board.unmake_move(after_clear[ply]);

    std::cout << "PASS: explicit per-node PV survives TT clearing and replays legally\n";
    return 0;
}
