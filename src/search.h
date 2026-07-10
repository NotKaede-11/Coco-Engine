#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <atomic>

const int INFINITY_SCORE = 50000;

namespace Search {
    // Timing parameters for active search
    extern uint64_t start_time;
    extern uint64_t target_time;
    extern uint64_t soft_limit;
    extern uint64_t hard_limit;
    extern std::atomic<bool> b_abort;
    extern uint64_t time_check_mask;

    // Thread count for Lazy SMP
    extern int num_threads;

    // Per-thread statistics
    struct ThreadStats {
        uint64_t nodes;
        int seldepth;
    };
    extern ThreadStats thread_stats[MAX_THREADS];

    // Search tuning options
    extern int RFP_Margin;
    extern int LMR_Constant_Scaled;
    extern int NMP_Base;
    extern int NMP_Divisor;
    extern int Aspiration_Delta;
    extern int History_Threshold;
    extern int Move_Overhead;

    // Initialize search-related tables (like LMR lookup table)
    void init_search_tables();

    // Allocate soft and hard search boundaries from active clock values (in ms)
    void allocate_time(int time_left, int increment);

    // Compute time controls from clock parameters (called before thread launch)
    void compute_time_controls(Color side, int max_depth, int wtime, int btime, int winc, int binc, int movetime);

    // Master search entry point (main thread only)
    void search_position(Board& board, int max_depth);

    // Helper thread search entry point (threads 1..N-1)
    void search_helper(Board& board, int max_depth, int thread_id);
}

#endif // SEARCH_H
