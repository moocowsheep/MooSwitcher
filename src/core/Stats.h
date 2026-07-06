#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace moo {

// Global registry of named counters/gauges. Register once (any thread),
// bump lock-free on hot paths, snapshot from the GUI/telemetry side.
class Stats {
public:
    class Counter {
    public:
        void add(int64_t d = 1) { v_.fetch_add(d, std::memory_order_relaxed); }
        void set(int64_t v) { v_.store(v, std::memory_order_relaxed); }
        int64_t value() const { return v_.load(std::memory_order_relaxed); }

    private:
        std::atomic<int64_t> v_{0};
    };

    // Returns a stable reference; creates the counter on first use.
    static Counter& counter(const std::string& name);

    struct Sample {
        std::string name;
        int64_t value;
    };
    static std::vector<Sample> snapshot();
};

}  // namespace moo
