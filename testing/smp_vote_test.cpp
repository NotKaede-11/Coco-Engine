#include "../src/search.h"

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void publish(int thread, Move move, int score, int depth) {
    Search::thread_stats[thread].completed_score.store(score);
    Search::thread_stats[thread].completed_depth.store(depth);
    Search::thread_stats[thread].completed_move.store(move.value);
}

} // namespace

int main() {
    const Move main_move(100);
    const Move helper_move(200);

    Search::num_threads = 1;
    publish(0, main_move, 20, 10);
    require(Search::test_select_smp_voted_move(main_move) == main_move,
            "one-thread voting changed the main result");

    Search::num_threads = 4;
    publish(0, main_move, 20, 10);
    publish(1, helper_move, 25, 12);
    publish(2, helper_move, 18, 11);
    publish(3, Move(), 500, 64);
    require(Search::test_select_smp_voted_move(main_move) == helper_move,
            "two agreeing deep helpers did not win the vote");

    publish(0, main_move, 20, 12);
    publish(1, helper_move, 20, 12);
    publish(2, Move(), 0, 0);
    publish(3, Move(), 0, 0);
    require(Search::test_select_smp_voted_move(main_move) == main_move,
            "a tied vote did not preserve the main-thread move");

    std::cout << "PASS: deterministic score/depth SMP voting\n";
    return 0;
}
