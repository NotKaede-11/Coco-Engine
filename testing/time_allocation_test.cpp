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

void require_sane_limits(uint64_t available, const char* context) {
    require(Search::soft_limit >= 1, context);
    require(Search::soft_limit <= Search::hard_limit, context);
    require(Search::hard_limit <= available, context);
}

} // namespace

int main() {
    Search::Move_Overhead = 30;

    Search::allocate_time(100, 0, 0);
    require_sane_limits(100, "100 ms clock produced unsafe limits");
    const uint64_t no_increment_soft = Search::soft_limit;

    Search::allocate_time(100, 5000, 0);
    require_sane_limits(100, "increment-dominated clock exceeded available time");
    require(Search::soft_limit >= no_increment_soft,
            "increment did not increase or preserve the soft budget");

    Search::allocate_time(10000, 0, 0);
    const uint64_t normal_no_increment_soft = Search::soft_limit;
    Search::allocate_time(10000, 100, 0);
    require_sane_limits(10000, "normal increment clock produced unsafe limits");
    require(Search::soft_limit > normal_no_increment_soft,
            "normal increment did not contribute to the soft budget");

    Search::allocate_time(20, 0, 0);
    require_sane_limits(20, "low clock produced unsafe limits");

    Search::Limits limits;
    limits.wtime = 0;
    limits.btime = 0;
    limits.winc = 5000;
    limits.binc = 5000;
    Search::compute_time_controls(WHITE, limits);
    require_sane_limits(1, "0+5 clock spent uncredited increment");

    limits = Search::Limits{};
    limits.movetime = 1;
    Search::compute_time_controls(WHITE, limits);
    require_sane_limits(1, "one-millisecond movetime was exceeded");

    std::cout << "PASS: increment and low-clock allocation safety\n";
    return 0;
}
