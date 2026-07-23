/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See EXCEPTIONS.md for the
 * full exception text. */

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
