#include "../src/search.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main() {
    require(Search::test_root_node_fraction_multiplier(0, 0) == 1.0,
            "empty accounting changed the time budget");
    require(Search::test_root_node_fraction_multiplier(70, 100) < 1.0,
            "dominant root move did not reduce the soft budget");
    require(Search::test_root_node_fraction_multiplier(10, 100) > 1.0,
            "uncertain root move did not extend the soft budget");
    require(std::abs(Search::test_root_node_fraction_multiplier(40, 100) - 1.0)
                < 1e-9,
            "ordinary root share changed the soft budget");
    require(Search::test_root_node_fraction_multiplier(101, 100) == 1.0,
            "invalid accounting changed the time budget");
    std::cout << "PASS: root node-fraction time scaling boundaries\n";
    return 0;
}
