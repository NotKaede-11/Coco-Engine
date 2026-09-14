#include "../src/search.h"

#include <cstdlib>
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    const auto low = Search::test_widen_aspiration(70, 82, 118, 18, true);
    require(low.alpha == 52, "fail-low did not recenter alpha on the score");
    require(low.beta == 100, "fail-low did not contract the stale upper side");
    require(low.delta == 27, "fail-low did not widen delta by 50 percent");

    const auto high = Search::test_widen_aspiration(130, 82, 118, 18, false);
    require(high.alpha == 82, "fail-high unexpectedly moved alpha");
    require(high.beta == 148, "fail-high did not recenter beta on the score");
    require(high.delta == 27, "fail-high did not widen delta by 50 percent");

    const auto floor = Search::test_widen_aspiration(
        -INFINITY_SCORE, -INFINITY_SCORE + 4, 0, 18, true);
    require(floor.alpha == -INFINITY_SCORE,
            "fail-low widening crossed the negative score bound");

    std::cout << "PASS: asymmetric aspiration recentering and score bounds\n";
    return 0;
}
