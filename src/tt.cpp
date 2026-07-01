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

TranspositionTable::TranspositionTable() : table(nullptr), num_entries(0) {
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
    num_entries = bytes / sizeof(TTEntry);
    
    // Allocate contiguous array
    table = static_cast<TTEntry*>(std::malloc(num_entries * sizeof(TTEntry)));
    clear();
}

void TranspositionTable::clear() {
    if (table && num_entries > 0) {
        std::memset(table, 0, num_entries * sizeof(TTEntry));
    }
}

void TranspositionTable::store(U64 key, Move best_move, int score, uint8_t depth, uint8_t flag, int ply) {
    if (!table || num_entries == 0) return;
    
    // Simple direct mapping hash index
    size_t index = key % num_entries;
    
    // Store if entry is empty, or if we find a deeper/equal result (depth-preferred replacement)
    if (table[index].key == 0 || depth >= table[index].depth) {
        table[index].key = key;
        table[index].best_move = best_move;
        table[index].score = score_to_tt(score, ply);
        table[index].depth = depth;
        table[index].flag = flag;
    }
}

bool TranspositionTable::probe(U64 key, int& score, Move& best_move, uint8_t depth, int alpha, int beta, int ply) {
    if (!table || num_entries == 0) return false;
    
    size_t index = key % num_entries;
    
    // Verify key match to confirm cache hit
    if (table[index].key == key) {
        best_move = table[index].best_move;
        
        // We can only use the cached score if the search depth when saved was sufficient
        if (table[index].depth >= depth) {
            int tt_score = score_from_tt(table[index].score, ply);
            
            if (table[index].flag == HASH_EXACT) {
                score = tt_score;
                return true;
            }
            if (table[index].flag == HASH_ALPHA && tt_score <= alpha) {
                score = alpha;
                return true;
            }
            if (table[index].flag == HASH_BETA && tt_score >= beta) {
                score = beta;
                return true;
            }
        }
    }
    
    return false;
}
