#include "../src/board.h"
#include "../src/movegen.h"
#include "../src/nnue.h"
#include "../src/search.h"
#include "../src/tt.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr int MATE = 30000;
constexpr int MATE_BAND = 29000;

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int exact_shallow_mate(Board& board, int remaining, int ply,
                       std::unordered_map<U64, int>& cache) {
    if (ply > 0 && (board.get_halfmove_clock() >= 100 || board.is_repetition()))
        return 0;

    const U64 cache_key = board.get_hash_key()
        ^ (static_cast<U64>(remaining) << 56);
    if (const auto found = cache.find(cache_key); found != cache.end())
        return found->second;

    MoveList moves;
    generate_pseudo_legal_moves(board, moves);
    int legal = 0;
    int best = -MATE;
    for (int index = 0; index < moves.count; ++index) {
        const Move move = moves.moves[index];
        if (!board.make_move(move)) continue;
        ++legal;
        const int child = remaining > 0
            ? exact_shallow_mate(board, remaining - 1, ply + 1, cache)
            : 0;
        const int score = -child;
        board.unmake_move(move);
        best = std::max(best, score);
    }

    if (legal == 0) {
        const Color side = board.get_side_to_move();
        const U64 king = board.get_pieces(side, KING);
        const bool checked = king
            && board.is_square_attacked(get_lsb(king), side ^ 1);
        const int terminal = checked ? -MATE + ply : 0;
        cache.emplace(cache_key, terminal);
        return terminal;
    }
    const int result = remaining == 0 ? 0 : best;
    cache.emplace(cache_key, result);
    return result;
}

bool is_checkmate(Board& board) {
    MoveList moves;
    generate_pseudo_legal_moves(board, moves);
    for (int index = 0; index < moves.count; ++index) {
        if (board.make_move(moves.moves[index])) {
            board.unmake_move(moves.moves[index]);
            return false;
        }
    }
    const Color side = board.get_side_to_move();
    const U64 king = board.get_pieces(side, KING);
    return king && board.is_square_attacked(get_lsb(king), side ^ 1);
}

}  // namespace

int main() {
    Board::init_zobrist();
    init_all_attack_tables();
    Search::init_search_tables();
    tt.resize(16);
    require(g_nnue.load_network("coco.nnue"), "load NNUE");

    const std::vector<std::string> fens = {
        "7k/6Q1/6K1/8/8/8/8/8 b - - 0 1",
        "7k/5Q2/6K1/8/8/8/8/8 w - - 0 1",
        "7k/8/5QK1/8/8/8/8/8 b - - 0 1",
        "8/7k/5Q2/6K1/8/8/8/8 b - - 0 1",
        "6k1/8/5QK1/8/8/8/8/8 w - - 0 1",
        "8/6k1/8/5QK1/8/8/8/8 w - - 0 1",
        "k7/8/1QK5/8/8/8/8/8 w - - 0 1",
        "8/k7/8/1QK5/8/8/8/8 w - - 0 1",
        "1k6/8/2KQ4/8/8/8/8/8 b - - 0 1",
        "8/1k6/8/2KQ4/8/8/8/8 b - - 0 1",
    };

    int wins = 0;
    int losses = 0;
    Search::mate_limit = 5;  // Disable heuristic pruning for the mate oracle.
    for (const std::string& fen : fens) {
        std::cout << "Checking mate fixture: " << fen << std::endl;
        Board board;
        require(board.parse_fen(fen), "parse mate fixture: " + fen);
        Board oracle = board;
        std::unordered_map<U64, int> cache;
        const int expected = exact_shallow_mate(oracle, 5, 0, cache);
        if (std::abs(expected) <= MATE_BAND) continue;

        tt.clear();
        const int observed = Search::test_alpha_beta_pv(board, -32000, 32000, 5);
        require(observed == expected,
                "mate distance mismatch for " + fen + ": expected "
                + std::to_string(expected) + ", observed " + std::to_string(observed));
        require(board.get_hash_key() == oracle.get_hash_key(), "mate search must restore board");

        Move pv[32]{};
        const int pv_length = Search::test_get_pv(board, pv, 32);
        const int mate_plies = MATE - std::abs(expected);
        require(pv_length >= mate_plies,
                "mate PV is truncated for " + fen + ": expected at least "
                + std::to_string(mate_plies) + " plies, got " + std::to_string(pv_length));
        for (int ply = 0; ply < mate_plies; ++ply)
            require(board.make_move(pv[ply]), "mate PV contains an illegal move");
        require(is_checkmate(board), "mate PV does not terminate in checkmate");
        for (int ply = mate_plies - 1; ply >= 0; --ply) board.unmake_move(pv[ply]);
        wins += expected > 0;
        losses += expected < 0;
    }
    Search::mate_limit = 0;

    std::cout << "Mate oracle coverage: wins=" << wins << " losses=" << losses << '\n';
    require(wins >= 4, "fixture set must cover at least four shortest winning mates");
    require(losses >= 3, "fixture set must cover at least three longest-resistance losses");
    std::cout << "PASS: mate-distance scoring/PVs and " << wins << " winning/" << losses
              << " losing exhaustive shallow mate fixtures\n";
    return 0;
}
