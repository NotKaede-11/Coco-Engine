#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"

namespace Search {
    // Timing parameters for active search
    extern uint64_t start_time;
    extern uint64_t target_time;
    extern uint64_t soft_limit;
    extern uint64_t hard_limit;
    extern bool b_abort;

    // Search tuning options
    extern int RFP_Margin;
    extern int LMR_Constant_Scaled;
    extern int NMP_Base;
    extern int NMP_Divisor;
    extern int Aspiration_Delta;
    extern int History_Threshold;

    // Initialize search-related tables (like LMR lookup table)
    void init_search_tables();

    // Allocate soft and hard search boundaries from active clock values (in ms)
    void allocate_time(int time_left, int increment);

    // Master search entry point supporting maximum depth and clock parameters (in ms)
    void search_position(Board& board, int max_depth, int wtime = -1, int btime = -1, int winc = 0, int binc = 0, int movetime = -1);
}

#endif // SEARCH_H
