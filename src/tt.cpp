#include "tt.h"
#include <cstdlib>
#include <cstring>

// Constants for mate score adjustments
const int MATE_SCORE = 30000;
const int MATE_THRESHOLD = 29000;

// Instantiate the global transposition table
TranspositionTable tt;

inline int score_to_tt(int score, int ply) {
    if (score > MATE_THRESHOLD) return score + ply;
    if (score < -MATE_THRESHOLD) return score - ply;
    return score;
}

inline int score_from_tt(int score, int ply) {
    if (score > MATE_THRESHOLD) return score - ply;
    if (score < -MATE_THRESHOLD) return score + ply;
    return score;
}

TranspositionTable::TranspositionTable() : table(nullptr), num_buckets(0) {
    // Default initialization to 16MB
    resize(16);
}

TranspositionTable::~TranspositionTable() {
    if (table) {
        std::free(table);
    }
}

void TranspositionTable::resize(size_t mb) {
    if (table) {
        std::free(table);
        table = nullptr;
    }
    
    // Calculate number of elements that fit in requested memory size
    size_t bytes = mb * 1024 * 1024;
    num_buckets = bytes / sizeof(TTBucket);
    
    // Allocate contiguous array
    table = static_cast<TTBucket*>(std::malloc(num_buckets * sizeof(TTBucket)));
    clear();
}

void TranspositionTable::clear() {
    if (table && num_buckets > 0) {
        std::memset(table, 0, num_buckets * sizeof(TTBucket));
    }
}

void TranspositionTable::store(U64 key, Move best_move, int score, uint8_t depth, uint8_t flag, int ply) {
    if (!table || num_buckets == 0) return;
    
    // Simple direct mapping hash index
    size_t index = key % num_buckets;
    TTBucket& bucket = table[index];
    
    int score_adjusted = score_to_tt(score, ply);
    
    // 1. If key matches an existing entry, overwrite it
    if (bucket.entries[0].key == key) {
        bucket.entries[0] = { key, best_move, score_adjusted, depth, flag };
        return;
    }
    if (bucket.entries[1].key == key) {
        bucket.entries[1] = { key, best_move, score_adjusted, depth, flag };
        return;
    }
    
    // 2. Otherwise, replace based on depth-preferred rule
    // entries[0] is depth-preferred. We replace it if the new depth is greater/equal
    // or if its key is empty (0).
    if (bucket.entries[0].key == 0 || depth >= bucket.entries[0].depth) {
        // Move old depth-preferred to always-replace slot (so we don't lose it entirely!)
        bucket.entries[1] = bucket.entries[0];
        bucket.entries[0] = { key, best_move, score_adjusted, depth, flag };
    } else {
        // Replace the always-replace slot
        bucket.entries[1] = { key, best_move, score_adjusted, depth, flag };
    }
}

bool TranspositionTable::probe(U64 key, int& score, Move& best_move, uint8_t depth, int alpha, int beta, int ply) {
    if (!table || num_buckets == 0) return false;
    
    size_t index = key % num_buckets;
    TTBucket& bucket = table[index];
    
    // Verify key match across the 2 slots
    for (int i = 0; i < 2; i++) {
        if (bucket.entries[i].key == key) {
            best_move = bucket.entries[i].best_move;
            
            // We can only use the cached score if the search depth when saved was sufficient
            if (bucket.entries[i].depth >= depth) {
                int tt_score = score_from_tt(bucket.entries[i].score, ply);
                
                if (bucket.entries[i].flag == HASH_EXACT) {
                    score = tt_score;
                    return true;
                }
                if (bucket.entries[i].flag == HASH_ALPHA && tt_score <= alpha) {
                    score = alpha;
                    return true;
                }
                if (bucket.entries[i].flag == HASH_BETA && tt_score >= beta) {
                    score = beta;
                    return true;
                }
            }
            break; // Found matching key, no need to check other slot
        }
    }
    
    return false;
}

bool TranspositionTable::probe_entry(U64 key, int& score, uint8_t& depth, uint8_t& flag, Move& best_move, int ply) {
    if (!table || num_buckets == 0) return false;
    
    size_t index = key % num_buckets;
    TTBucket& bucket = table[index];
    
    for (int i = 0; i < 2; i++) {
        if (bucket.entries[i].key == key) {
            best_move = bucket.entries[i].best_move;
            score = score_from_tt(bucket.entries[i].score, ply);
            depth = bucket.entries[i].depth;
            flag = bucket.entries[i].flag;
            return true;
        }
    }
    
    return false;
}

void TranspositionTable::prefetch(U64 key) const {
#if defined(__GNUC__) || defined(__clang__)
    if (table && num_buckets > 0) {
        __builtin_prefetch(&table[key % num_buckets]);
    }
#endif
}
