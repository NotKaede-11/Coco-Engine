#pragma once

#include <array>
#include <cstdint>

namespace CocoPolicy {

enum class Profile {
    PZ59, PZ61, Calvin, Rice, Pawnstar, Askaig, Alexandria, Maelstrom, Stockfish18, Stockfish19
};
inline constexpr Profile profile = Profile::Calvin;

struct Budget {
    double soft;
    double hard;
};
struct GameClock {
    double first_clock_factor = -1;
    double previous_time_reduction = 1;
    int previous_score = 0;
    bool have_score = false;
};
struct RootIteration {
    int depth;
    int score;
    int last_score;
    int initial_eval;
    int move_stability;
    int score_stability;
    bool all_moves_stable;
    bool all_scores_stable;
    bool quiet_best;
    bool first_move_best;
    bool single_move;
    uint64_t nodes;
    double best_node_fraction;
};
Budget allocate_clock(int remaining, int increment, int moves_to_go, int game_ply,
                      int overhead, bool ponder, GameClock& game);
double soft_multiplier(const RootIteration& root, const GameClock& game, double& time_reduction);

struct Aspiration {
    int depth;
    int delta;
};
Aspiration aspiration(int last_score);
void widen(int score, int previous_score, bool fail_low, int& alpha, int& beta, int& delta);

struct ProbCut {
    int depth;
    int margin;
    int reduction;
    bool use_tt_veto;
    bool skip_tt_move;
};
ProbCut probcut(bool improving);
bool custom_singular();
int singular_min_depth(bool pv);
int singular_margin(int depth, bool pv);
int singular_depth(int depth);
int singular_extension(int score, int beta, int depth, bool pv, int prior_extra);
int multicut_score(int score, int beta, int singular_beta);

} // namespace CocoPolicy
