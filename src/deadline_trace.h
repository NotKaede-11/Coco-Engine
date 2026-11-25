#ifndef COCO_DEADLINE_TRACE_H
#define COCO_DEADLINE_TRACE_H

// Opt-in diagnostics only. Normal builds have no storage, clock reads or output.
#ifdef COCO_DEADLINE_TRACE
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>

namespace DeadlineTrace {
enum Phase { Received, Joined, Prepared, Launched, Entered, Initialized,
             Deadline, Finishing, HelpersDone, Reply, PhaseCount };
inline std::array<std::atomic<long long>, PhaseCount> stamps{};
inline long long now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void mark(Phase phase) { stamps[phase].store(now(), std::memory_order_relaxed); }
// Called only after all previous search threads have been joined.
inline void reset(long long received) {
    for (auto& stamp : stamps) stamp.store(0, std::memory_order_relaxed);
    stamps[Received].store(received, std::memory_order_relaxed);
    mark(Joined);
}
inline void report(unsigned long long hard_ms) {
    static constexpr const char* names[] = {
        "received", "joined", "prepared", "launched", "entered", "initialized",
        "deadline", "finishing", "helpers_done", "reply"};
    const auto origin = stamps[Received].load(std::memory_order_relaxed);
    if (origin == 0) return; // Direct bench/test entry, not UCI go.
    std::cerr << "deadline_trace hard_ms=" << hard_ms;
    for (int i = 0; i < PhaseCount; ++i) {
        const auto value = stamps[i].load(std::memory_order_relaxed);
        std::cerr << ' ' << names[i] << "_us="
                  << (value == 0 ? -1 : (value - origin) / 1000);
    }
    std::cerr << '\n';
}
}
#define COCO_DEADLINE_MARK(phase) DeadlineTrace::mark(DeadlineTrace::phase)
#else
#define COCO_DEADLINE_MARK(phase) ((void)0)
#endif
#endif
