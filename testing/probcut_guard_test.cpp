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

Search::ProbCutTestResult probe(const char* fen, int depth,
                                Search::NodeType type = Search::NodeType::NON_PV) {
    Board board;
    require(board.parse_fen(fen), "fixture FEN must parse");
    tt.clear();
    return Search::test_probcut_window(board, -200, -100, depth, type);
}

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    tt.resize(16);
    if (!g_nnue.load_network("coco.nnue")) return 1;

    constexpr const char* tactical =
        "4k3/8/8/8/3Q4/8/3r4/K7 w - - 0 1";
    const auto active = probe(tactical, 5);
    Board after_capture;
    require(after_capture.parse_fen("4k3/8/8/8/8/8/3Q4/K7 b - - 0 1"),
            "post-capture FEN must parse");
    tt.clear();
    const int verification = Search::test_alpha_beta_window(after_capture, -100, -99, 1);
    std::cout << "active score=" << active.score
              << " attempts=" << active.attempts
              << " cutoffs=" << active.cutoffs
              << " probe_nodes=" << active.probe_nodes
              << " last_probe_score=" << active.last_probe_score
              << " direct_verification=" << verification << '\n';
    require(active.attempts > 0,
            "capture-only ProbCut must inspect a legal SEE-winning capture");
    require(active.cutoffs > 0,
            "the winning-capture fixture must produce a ProbCut cutoff");
    require(active.probe_nodes > 0,
            "ProbCut must report nodes spent in its verification searches");

    const auto pv = probe(tactical, 6, Search::NodeType::PV);
    require(pv.attempts == 0, "ProbCut must stay disabled at PV nodes");

    const auto shallow = probe(tactical, 4);
    require(shallow.attempts == 0, "ProbCut must stay disabled below its minimum depth");

    const auto checked = probe(
        "4k3/8/8/8/3Q4/8/4r3/4K3 w - - 0 1", 6);
    require(checked.attempts == 0, "ProbCut must stay disabled while in check");

    const auto quiet = probe("4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1", 6);
    require(quiet.attempts == 0,
            "capture-only ProbCut must not manufacture quiet candidates");

    std::cout << "PASS: ProbCut trigger, PV/check/depth guards, capture-only scope, and counters\n";
    return 0;
}
