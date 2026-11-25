// Coco's selected, fixed search/clock policy. See third_party/NOTICES.md
// and the release credit audit for the Calvin reference and source-scope review.
#include "search_policy.h"
#include "search.h"
#include <algorithm>
#include <cmath>

namespace CocoPolicy {
Budget allocate_clock(int remaining, int increment, int, int, int overhead, bool, GameClock&) {
    const double time = std::max(1, remaining);
    const double added = std::max(0, increment);
    const double base = 0.05 * time + 0.75 * added;
    double ceiling = std::max(1.0, time - std::min<double>(overhead, time / 2));
    // Leave room for scheduling delays before the next increment is earned.
    if (remaining <= 1000)
        ceiling = std::min(ceiling, std::max(1.0, time / 4));
    const double hard = std::clamp(2 * base - 50, 1.0, ceiling);
    return {std::clamp(0.66 * base, 1.0, hard), hard};
}

double soft_multiplier(const RootIteration& root, const GameClock&, double&) {
    constexpr double move_factor[]{2.50, 1.20, 0.90, 0.80, 0.75};
    constexpr double score_factor[]{1.25, 1.15, 1.00, 0.94, 0.88};
    const double share = std::clamp(root.best_node_fraction, 0.0, 1.0);
    return std::clamp(move_factor[std::min(root.move_stability, 4)]
                      * score_factor[std::min(root.score_stability, 4)]
                      * (root.depth >= 5 ? (1.5 - share) * 1.35 : 1), 0.12, 2.5);
}

Aspiration aspiration(int) { return {2, 12}; }

void widen(int, int, bool fail_low, int& alpha, int& beta, int& delta) {
    if (fail_low) {
        beta = (alpha + beta) / 2;
        alpha -= delta;
    } else {
        beta += delta;
    }
    delta *= 2;
    alpha = std::max(alpha, -INFINITY_SCORE);
    beta = std::min(beta, INFINITY_SCORE);
    if (delta >= INFINITY_SCORE / 4) {
        alpha = -INFINITY_SCORE;
        beta = INFINITY_SCORE;
    }
}

ProbCut probcut(bool) { return {5, 200, 4, false, false}; }
bool custom_singular() { return false; }
int singular_min_depth(bool pv) { return 6 + pv; }
int singular_margin(int depth, bool pv) { return depth * (pv ? 1 : 2); }
int singular_depth(int depth) { return depth / 2; }
int singular_extension(int score, int beta, int depth, bool pv, int prior_extra) {
    if (pv || prior_extra >= 6) return 1;
    return score < beta - 2 * depth ? 2 : 1;
}
int multicut_score(int score, int beta, int) {
    constexpr int limit = VALUE_TB - MAX_PLY;
    return std::abs(beta) < limit && std::abs(score) < limit
        ? std::min(score, beta + 64) : score;
}
}
