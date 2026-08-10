#ifndef SEARCH_H
#define SEARCH_H

#include "board.h"
#include <atomic>
#include <vector>

class TranspositionTable;

const int INFINITY_SCORE = 50000;

namespace Search {
    enum class NodeType : uint8_t { NON_PV, PV };

    // Complete set of UCI search constraints.  Keeping them together avoids
    // accidentally dropping a limit while crossing the UCI/thread boundary.
    struct Limits {
        int depth = 64;
        int wtime = -1;
        int btime = -1;
        int winc = 0;
        int binc = 0;
        int movetime = -1;
        int movestogo = 0;
        uint64_t nodes = 0;
        int mate = 0;
        bool infinite = false;
        bool ponder = false;
        bool searchmoves_specified = false;
        std::vector<Move> searchmoves;
    };

    // Timing parameters for active search
    extern std::atomic<uint64_t> start_time;
    extern uint64_t target_time;
    extern uint64_t soft_limit;
    extern uint64_t hard_limit;
    extern std::atomic<bool> b_abort;
    extern std::atomic<bool> pondering;
    extern std::atomic<int> active_helpers;
    extern uint64_t time_check_mask;
    extern uint64_t node_limit;
    extern int mate_limit;

    // Thread count for Lazy SMP
    extern int num_threads;

    // Per-thread statistics
    struct ThreadStats {
        std::atomic<uint64_t> nodes{0};
        std::atomic<uint64_t> tbhits{0};
        std::atomic<int> seldepth{0};
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
    extern int SyzygyProbeDepth;
    extern bool SyzygyProbeLimit;
    extern bool Syzygy50MoveRule;
    extern int LMR_History_Divisor;
    extern int Contempt;
    extern int SEE_Pruning_Depth;
    extern bool Ponder;
    extern int MultiPV;
    extern bool UCI_ShowWDL;
    extern bool UCI_AnalyseMode;

    // Initialize search-related tables (like LMR lookup table)
    void init_search_tables();

    // Allocate soft and hard search boundaries from active clock values (in ms)
    void allocate_time(int time_left, int increment, int moves_to_go);

    // Compute time controls from clock parameters (called before thread launch)
    void compute_time_controls(Color side, const Limits& limits);

    void ponder_hit();

    // Establish a stable legal-root-move index before Lazy SMP workers start.
    // Workers publish one aggregate node delta per completed root move.
    void reset_root_node_accounting(const Board& board);

    // Configure an optional UCI searchmoves allow-list before workers launch.
    void set_root_moves(const std::vector<Move>& moves, bool restricted);

    // Datagen workers bind a private TT and reset all thread-local heuristics
    // between games. Passing nullptr restores the normal shared UCI TT.
    void set_thread_tt(TranspositionTable* table);
    void reset_worker_search_state(Color root_side, int worker_id);

    // Master search entry point (main thread only)
    void search_position(Board& board, int max_depth);

    // Helper thread search entry point (threads 1..N-1)
    void search_helper(Board& board, int max_depth, int thread_id);

#ifdef COCO_TESTING
    struct NmpTestResult {
        int score;
        int attempts;
        int cutoffs;
    };
    int test_quiescence_window(Board& board, int alpha, int beta);
    int test_alpha_beta_window(Board& board, int alpha, int beta, int depth);
    int test_alpha_beta_pv(Board& board, int alpha, int beta, int depth);
    NmpTestResult test_nmp_window(Board& board, int alpha, int beta, int depth,
                                  NodeType node_type);
    uint64_t test_root_nodes_total();
    uint64_t test_root_nodes_for(Move move);
    int test_get_pv(Board& board, Move* pv, int max_depth);
    void test_score_to_wdl(int score, int& win, int& draw, int& loss);
#endif
}

int alpha_beta(Board& board, int alpha, int beta, int depth, int ply,
               Search::NodeType node_type, bool in_null_move_search = false,
               int parent_eval_1 = INFINITY_SCORE, int parent_eval_2 = INFINITY_SCORE,
               Move excluded_move = Move(), int double_ext = 0);

#endif // SEARCH_H
