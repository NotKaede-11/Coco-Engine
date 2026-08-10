#include "../src/tt.h"

#include <iostream>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition)
        std::cerr << "FAIL: " << message << '\n';
    return condition;
}

}  // namespace

int main() {
    TranspositionTable table;
    table.resize(1);

    int score = 0;
    Move move;
    const Move move_a(SQ_E2, SQ_E4, FLAG_DOUBLE_PAWN);
    const Move move_b(SQ_G1, SQ_F3, FLAG_QUIET);

    table.store(0x101ULL, move_a, 42, 8, HASH_EXACT, 0);
    if (!expect(table.probe(0x101ULL, score, move, 8, -10, 10, 0) &&
                score == 42 && move == move_a, "exact hit")) return 1;

    table.store(0x202ULL, move_a, -50, 8, HASH_ALPHA, 0);
    if (!expect(table.probe(0x202ULL, score, move, 8, 0, 100, 0) && score == -50,
                "upper bound must return stored score")) return 1;

    table.store(0x303ULL, move_b, 150, 8, HASH_BETA, 0);
    if (!expect(table.probe(0x303ULL, score, move, 8, 0, 100, 0) && score == 150,
                "lower bound must return stored score")) return 1;

    move = Move();
    if (!expect(!table.probe(0x303ULL, score, move, 9, 0, 100, 0) && move == move_b,
                "shallow entry must still expose its move")) return 1;

    table.store(0x404ULL, move_a, 10, 10, HASH_EXACT, 0);
    table.store(0x404ULL, Move(), 11, 11, HASH_EXACT, 0);
    uint8_t depth = 0, flag = 0;
    if (!expect(table.probe_entry(0x404ULL, score, depth, flag, move, 0) && move == move_a,
                "move-less same-key write must retain move")) return 1;

    constexpr int mate = 29990;
    table.store(0x505ULL, move_a, mate, 12, HASH_EXACT, 3);
    if (!expect(table.probe_entry(0x505ULL, score, depth, flag, move, 5) && score == mate - 2,
                "positive mate-distance conversion")) return 1;
    table.store(0x606ULL, move_a, -mate, 12, HASH_EXACT, 3);
    if (!expect(table.probe_entry(0x606ULL, score, depth, flag, move, 5) && score == -mate + 2,
                "negative mate-distance conversion")) return 1;

    table.clear();
    table.new_search();
    const U64 stride = static_cast<U64>(table.bucket_count());
    const U64 collision_base = 7;
    for (int slot = 0; slot < 4; ++slot)
        table.store(collision_base + stride * slot, move_a, slot, 20 - slot * 2,
                    HASH_EXACT, 0);
    if (!expect(table.hashfull() > 0, "hashfull after current-generation stores"))
        return 1;
    for (int slot = 0; slot < 4; ++slot)
        if (!expect(table.probe_entry(collision_base + stride * slot, score, depth,
                                      flag, move, 0), "four-entry collision retention"))
            return 1;

    for (int age = 0; age < 8; ++age)
        table.new_search();
    U64 fresh_key = collision_base + stride * 4;
    table.store(fresh_key, move_b, 77, 1, HASH_EXACT, 0);
    if (!expect(table.probe_entry(fresh_key, score, depth, flag, move, 0),
                "stale-generation replacement")) return 1;

    table.store(0, move_b, 9, 3, HASH_EXACT, 0);
    if (!expect(table.probe_entry(0, score, depth, flag, move, 0) &&
                score == 9 && move == move_b, "zero key support")) return 1;

    table.clear();
    table.new_search();
    std::atomic<bool> race_failure{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            U64 key = 3 + stride * static_cast<U64>(worker);
            Move expected(SQ_A2 + worker, SQ_A3 + worker, FLAG_QUIET);
            for (int iteration = 0; iteration < 20000; ++iteration) {
                table.store(key, expected, worker * 10, 6, HASH_EXACT, 0);
                int observed_score = 0;
                uint8_t observed_depth = 0, observed_flag = 0;
                Move observed_move;
                if (table.probe_entry(key, observed_score, observed_depth,
                                      observed_flag, observed_move, 0) &&
                    (observed_score != worker * 10 || observed_move != expected ||
                     observed_depth != 6 || observed_flag != HASH_EXACT))
                    race_failure.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers)
        worker.join();
    if (!expect(!race_failure.load(std::memory_order_relaxed),
                "lock-free concurrent verification")) return 1;

    table.clear();
    if (!expect(!table.probe_entry(0x101ULL, score, depth, flag, move, 0), "clear"))
        return 1;

    std::cout << "PASS: TT bounds, move retention, depth, mate conversion, and clear\n";
    return 0;
}
