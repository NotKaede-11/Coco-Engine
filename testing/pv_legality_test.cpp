#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/search.h"
#include "../src/tt.h"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <vector>

extern thread_local uint64_t nodes_visited;

namespace {

[[noreturn]] void fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

std::vector<Move> legal_moves(Board& board) {
    MoveList generated;
    generate_pseudo_legal_moves(board, generated);
    const LegalityMasks masks = board.get_legality_masks();
    std::vector<Move> legal;
    for (int i = 0; i < generated.count; ++i)
        if (board.is_move_legal(generated.moves[i], masks))
            legal.push_back(generated.moves[i]);
    return legal;
}

void validate_search(Board& board, int case_index) {
    const std::string original = board.get_fen();
    const std::vector<Move> roots = legal_moves(board);
    if (roots.empty()) return;

    tt.clear();
    Search::reset_root_node_accounting(board);
    Search::test_alpha_beta_window(board, -INFINITY_SCORE, INFINITY_SCORE, 3);
    if (board.get_fen() != original)
        fail("search failed to restore randomized root " + std::to_string(case_index));

    Move pv[64];
    const int length = Search::test_get_pv(board, pv, 64);
    if (length <= 0)
        fail("non-terminal randomized root has an empty PV " + std::to_string(case_index));

    for (int ply = 0; ply < length; ++ply) {
        const std::vector<Move> moves = legal_moves(board);
        bool found = false;
        for (Move move : moves)
            if (move == pv[ply]) { found = true; break; }
        if (!found || !board.make_move(pv[ply], true))
            fail("illegal PV move at case " + std::to_string(case_index)
                 + " ply " + std::to_string(ply));
    }
    for (int ply = length - 1; ply >= 0; --ply) board.unmake_move(pv[ply]);
    if (board.get_fen() != original)
        fail("PV replay failed to restore randomized root " + std::to_string(case_index));

    const uint64_t attributed = Search::test_root_nodes_total();
    if (attributed == 0 || attributed > nodes_visited)
        fail("invalid aggregate root-node accounting at case " + std::to_string(case_index));
    if (Search::test_root_nodes_for(pv[0]) == 0)
        fail("PV root move has no attributed nodes at case " + std::to_string(case_index));
}

}  // namespace

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    Search::num_threads = 1;
    tt.resize(16);
    if (!g_nnue.load_network("coco.nnue")) return 1;

    std::mt19937 rng(0xC0C015u);
    Board board;
    constexpr const char* start =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    for (int case_index = 0; case_index < 96; ++case_index) {
        if (!board.parse_fen(start)) fail("could not parse start position");
        const int target_plies = 4 + static_cast<int>(rng() % 48);
        for (int ply = 0; ply < target_plies; ++ply) {
            std::vector<Move> moves = legal_moves(board);
            if (moves.empty()) break;
            Move selected = moves[rng() % moves.size()];
            if (!board.make_move(selected, true)) fail("random legal move was rejected");
        }
        validate_search(board, case_index);
    }

    std::cout << "PASS: 96 randomized PV legality/replay and root-accounting oracles\n";
    return 0;
}
