#ifndef TT_H
#define TT_H

#include "types.h"

#include <atomic>
#include <cstddef>

enum TTFlag : uint8_t {
    HASH_EXACT = 0,
    HASH_ALPHA = 1,
    HASH_BETA = 2
};

// Two atomic words permit lock-free validation: verification == key XOR data.
struct TTEntry {
    std::atomic<U64> verification{0};
    std::atomic<U64> data{0};
};

struct alignas(64) TTBucket {
    TTEntry entries[4];
};

static_assert(sizeof(TTEntry) == 16);
static_assert(sizeof(TTBucket) == 64);

class TranspositionTable {
public:
    TranspositionTable();
    ~TranspositionTable();

    TranspositionTable(const TranspositionTable&) = delete;
    TranspositionTable& operator=(const TranspositionTable&) = delete;

    void resize(size_t mb);
    void clear();
    void new_search();
    int hashfull() const;

    void store(U64 key, Move best_move, int score, uint8_t depth, uint8_t flag, int ply);
    bool probe(U64 key, int& score, Move& best_move, uint8_t depth,
               int alpha, int beta, int ply);
    bool probe_entry(U64 key, int& score, uint8_t& depth, uint8_t& flag,
                     Move& best_move, int ply);
    void prefetch(U64 key) const;

    size_t bucket_count() const { return num_buckets; }

private:
    TTBucket* table;
    size_t num_buckets;
    size_t index_mask;
    std::atomic<uint8_t> generation;
};

extern TranspositionTable tt;

#endif // TT_H
