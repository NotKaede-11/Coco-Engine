#ifndef TT_H
#define TT_H

#include "types.h"

// Hash flags for Transposition Table entries
enum TTFlag : uint8_t {
    HASH_EXACT = 0, // Exact evaluation score
    HASH_ALPHA = 1, // Upper bound (score <= alpha)
    HASH_BETA = 2  // Lower bound (score >= beta)
};

// Transposition Table Entry Structure
struct TTEntry {
    U64 key;         // 64-bit Zobrist key
    Move best_move;  // Best move found in this position (16-bit)
    int score;       // Score associated with the position
    uint8_t depth;   // Depth of the search from this node
    uint8_t flag;    // Bound type flag (exact, alpha, beta)
};

// TT Bucket containing 2 entries for Two-Tier depth-preferred/always-replace logic
struct TTBucket {
    TTEntry entries[2];
};

// Transposition Table Class
class TranspositionTable {
public:
    TranspositionTable();
    ~TranspositionTable();

    // Resize the table to given size in Megabytes
    void resize(size_t mb);

    // Clear all entries
    void clear();

    // Store a search result in the table (depth-preferred replacement)
    void store(U64 key, Move best_move, int score, uint8_t depth, uint8_t flag, int ply);

    // Probe the table for a stored evaluation score and best move
    bool probe(U64 key, int& score, Move& best_move, uint8_t depth, int alpha, int beta, int ply);

    // Probe the table for raw stored entry metrics (depth, score, flag, best_move)
    bool probe_entry(U64 key, int& score, uint8_t& depth, uint8_t& flag, Move& best_move, int ply);

    // Prefetch a table entry to minimize memory cache misses
    void prefetch(U64 key) const;

private:
    TTBucket* table;
    size_t num_buckets;
};

// Global transposition table instance
extern TranspositionTable tt;

#endif // TT_H
