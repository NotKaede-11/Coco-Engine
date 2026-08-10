#include "tt.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <new>

namespace {

constexpr int MATE_THRESHOLD = 29000;
constexpr U64 VALID_BIT = 1ULL << 50;

struct EntrySnapshot {
    U64 data = 0;
    Move move;
    int score = 0;
    uint8_t depth = 0;
    uint8_t flag = HASH_EXACT;
    uint8_t generation = 0;
    bool valid = false;
};

int score_to_tt(int score, int ply) {
    if (score > MATE_THRESHOLD) return score + ply;
    if (score < -MATE_THRESHOLD) return score - ply;
    return score;
}

int score_from_tt(int score, int ply) {
    if (score > MATE_THRESHOLD) return score - ply;
    if (score < -MATE_THRESHOLD) return score + ply;
    return score;
}

U64 pack_entry(Move move, int score, uint8_t depth, uint8_t flag, uint8_t generation) {
    int16_t packed_score = static_cast<int16_t>(std::clamp(score, -32767, 32767));
    return static_cast<U64>(move.value)
         | (static_cast<U64>(static_cast<uint16_t>(packed_score)) << 16)
         | (static_cast<U64>(depth) << 32)
         | (static_cast<U64>(flag & 0x3) << 40)
         | (static_cast<U64>(generation) << 42)
         | VALID_BIT;
}

EntrySnapshot decode_entry(U64 data) {
    EntrySnapshot result;
    result.data = data;
    result.valid = (data & VALID_BIT) != 0;
    if (!result.valid) return result;
    result.move = Move(static_cast<uint16_t>(data));
    result.score = static_cast<int16_t>((data >> 16) & 0xFFFF);
    result.depth = static_cast<uint8_t>((data >> 32) & 0xFF);
    result.flag = static_cast<uint8_t>((data >> 40) & 0x3);
    result.generation = static_cast<uint8_t>((data >> 42) & 0xFF);
    return result;
}

bool read_stable(const TTEntry& entry, U64& verification, U64& data) {
    U64 first_verification = entry.verification.load(std::memory_order_acquire);
    U64 observed_data = entry.data.load(std::memory_order_acquire);
    U64 second_verification = entry.verification.load(std::memory_order_acquire);
    if (first_verification != second_verification) return false;
    verification = first_verification;
    data = observed_data;
    return true;
}

bool read_matching(const TTEntry& entry, U64 key, EntrySnapshot& snapshot) {
    U64 verification = 0, data = 0;
    if (!read_stable(entry, verification, data)) return false;
    snapshot = decode_entry(data);
    return snapshot.valid && (verification ^ data) == key;
}

EntrySnapshot read_for_replacement(const TTEntry& entry) {
    U64 verification = 0, data = 0;
    if (!read_stable(entry, verification, data)) return {};
    return decode_entry(data);
}

void write_entry(TTEntry& entry, U64 key, U64 data) {
    entry.data.store(data, std::memory_order_relaxed);
    entry.verification.store(key ^ data, std::memory_order_release);
}

}  // namespace

TranspositionTable tt;

TranspositionTable::TranspositionTable()
    : table(nullptr), num_buckets(0), index_mask(0), generation(0) {
    resize(16);
}

TranspositionTable::~TranspositionTable() {
    delete[] table;
}

void TranspositionTable::resize(size_t mb) {
    delete[] table;
    table = nullptr;
    num_buckets = 0;
    index_mask = 0;

    size_t bytes = std::max<size_t>(mb, 1) * 1024 * 1024;
    size_t raw_buckets = std::max<size_t>(bytes / sizeof(TTBucket), 1);
    num_buckets = std::bit_floor(raw_buckets);
    index_mask = num_buckets - 1;
    table = new (std::nothrow) TTBucket[num_buckets];
    if (!table) {
        num_buckets = 0;
        index_mask = 0;
        return;
    }
    clear();
}

void TranspositionTable::clear() {
    if (!table) return;
    for (size_t bucket = 0; bucket < num_buckets; ++bucket)
        for (TTEntry& entry : table[bucket].entries) {
            entry.verification.store(0, std::memory_order_relaxed);
            entry.data.store(0, std::memory_order_relaxed);
        }
    generation.store(0, std::memory_order_relaxed);
}

void TranspositionTable::new_search() {
    generation.fetch_add(1, std::memory_order_relaxed);
}

int TranspositionTable::hashfull() const {
    if (!table || num_buckets == 0) return 0;
    // Sample at most 1000 entries (250 four-entry buckets), as UCI hashfull
    // is a per-mille estimate and should stay cheap on every info line.
    size_t sampled_buckets = std::min<size_t>(num_buckets, 250);
    int used = 0;
    uint8_t current = generation.load(std::memory_order_relaxed);
    for (size_t bucket = 0; bucket < sampled_buckets; ++bucket)
        for (const TTEntry& entry : table[bucket].entries) {
            EntrySnapshot snapshot = read_for_replacement(entry);
            used += snapshot.valid && snapshot.generation == current;
        }
    return static_cast<int>(used * 1000 / (sampled_buckets * 4));
}

void TranspositionTable::store(U64 key, Move best_move, int score,
                               uint8_t depth, uint8_t flag, int ply) {
    if (!table || num_buckets == 0) return;
    assert(flag <= HASH_BETA);

    TTBucket& bucket = table[key & index_mask];
    uint8_t current = generation.load(std::memory_order_relaxed);
    int adjusted_score = score_to_tt(score, ply);

    TTEntry* replacement = &bucket.entries[0];
    EntrySnapshot replacement_snapshot = read_for_replacement(*replacement);
    int replacement_value = replacement_snapshot.valid
        ? static_cast<int>(replacement_snapshot.depth)
            - 4 * static_cast<uint8_t>(current - replacement_snapshot.generation)
            + (replacement_snapshot.flag == HASH_EXACT ? 2 : 0)
        : -100000;

    for (TTEntry& entry : bucket.entries) {
        EntrySnapshot existing;
        if (read_matching(entry, key, existing)) {
            Move retained_move = best_move.is_none() ? existing.move : best_move;
            uint8_t age = static_cast<uint8_t>(current - existing.generation);

            // Preserve substantially deeper current-generation information,
            // while still allowing an independent move hint to be refreshed.
            if (age == 0 && flag != HASH_EXACT && depth + 4 < existing.depth) {
                if (!best_move.is_none() && best_move != existing.move) {
                    U64 data = pack_entry(retained_move, existing.score,
                                          existing.depth, existing.flag, current);
                    write_entry(entry, key, data);
                }
                return;
            }

            U64 data = pack_entry(retained_move, adjusted_score, depth, flag, current);
            write_entry(entry, key, data);
            return;
        }

        EntrySnapshot candidate = read_for_replacement(entry);
        int candidate_value = candidate.valid
            ? static_cast<int>(candidate.depth)
                - 4 * static_cast<uint8_t>(current - candidate.generation)
                + (candidate.flag == HASH_EXACT ? 2 : 0)
            : -100000;
        if (candidate_value < replacement_value) {
            replacement = &entry;
            replacement_snapshot = candidate;
            replacement_value = candidate_value;
        }
    }

    U64 data = pack_entry(best_move, adjusted_score, depth, flag, current);
    write_entry(*replacement, key, data);
}

bool TranspositionTable::probe(U64 key, int& score, Move& best_move,
                               uint8_t depth, int alpha, int beta, int ply) {
    if (!table || num_buckets == 0) return false;
    const TTBucket& bucket = table[key & index_mask];
    for (const TTEntry& entry : bucket.entries) {
        EntrySnapshot snapshot;
        if (!read_matching(entry, key, snapshot)) continue;
        best_move = snapshot.move;
        if (snapshot.depth < depth) return false;
        int tt_score = score_from_tt(snapshot.score, ply);
        if (snapshot.flag == HASH_EXACT ||
            (snapshot.flag == HASH_ALPHA && tt_score <= alpha) ||
            (snapshot.flag == HASH_BETA && tt_score >= beta)) {
            score = tt_score;
            return true;
        }
        return false;
    }
    return false;
}

bool TranspositionTable::probe_entry(U64 key, int& score, uint8_t& depth,
                                     uint8_t& flag, Move& best_move, int ply) {
    if (!table || num_buckets == 0) return false;
    const TTBucket& bucket = table[key & index_mask];
    for (const TTEntry& entry : bucket.entries) {
        EntrySnapshot snapshot;
        if (!read_matching(entry, key, snapshot)) continue;
        best_move = snapshot.move;
        score = score_from_tt(snapshot.score, ply);
        depth = snapshot.depth;
        flag = snapshot.flag;
        return true;
    }
    return false;
}

void TranspositionTable::prefetch(U64 key) const {
#if defined(__GNUC__) || defined(__clang__)
    if (table && num_buckets > 0)
        __builtin_prefetch(&table[key & index_mask]);
#endif
}
