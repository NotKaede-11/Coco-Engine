#include "../src/search.h"
#include <cstdlib>
#include <iostream>

int main() {
    int previous_win = -1;
    int previous_loss = 1001;
    for (int score = -30000; score <= 30000; score += 25) {
        int win = 0, draw = 0, loss = 0;
        Search::test_score_to_wdl(score, win, draw, loss);
        if (win < 0 || draw < 0 || loss < 0 || win + draw + loss != 1000) {
            std::cerr << "FAIL: invalid WDL tuple at " << score << '\n';
            return 1;
        }
        if (win < previous_win || loss > previous_loss) {
            std::cerr << "FAIL: non-monotonic WDL tuple at " << score << '\n';
            return 1;
        }
        previous_win = win;
        previous_loss = loss;
    }
    int win = 0, draw = 0, loss = 0;
    Search::test_score_to_wdl(29999, win, draw, loss);
    if (win != 1000 || draw != 0 || loss != 0) return 1;
    Search::test_score_to_wdl(-29999, win, draw, loss);
    if (win != 0 || draw != 0 || loss != 1000) return 1;
    std::cout << "PASS: WDL permille is bounded, sums to 1000, monotonic, and mate-safe\n";
    return 0;
}
