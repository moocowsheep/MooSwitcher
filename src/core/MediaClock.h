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
#include <cstdint>

namespace moo {

// Rational-rate media clock. Tick n fires at origin + n * (fpsD / fpsN) seconds.
// All conversions are exact integer arithmetic: no float drift over any runtime.
class MediaClock {
public:
    explicit MediaClock(int64_t fpsN = 60000, int64_t fpsD = 1001);

    void start();                    // origin = now (CLOCK_MONOTONIC)
    void startAt(int64_t originNs);  // explicit origin (tests, derived timelines)

    int64_t fpsN() const { return fpsN_; }
    int64_t fpsD() const { return fpsD_; }
    int64_t originNs() const { return originNs_; }

    // Absolute CLOCK_MONOTONIC ns at which tick n fires.
    int64_t nsForTick(int64_t n) const;
    // Largest n such that nsForTick(n) <= ns (negative before origin).
    int64_t tickForNs(int64_t ns) const;

    int64_t frameDurationNs() const;  // rounded; for display/estimates only

    static int64_t nowNs();  // CLOCK_MONOTONIC
    int64_t currentTick() const { return tickForNs(nowNs()); }

    // Sleeps until tick n (absolute deadline, EINTR-safe).
    // Returns false if the deadline had already passed when called.
    bool sleepUntilTick(int64_t n) const;

private:
    int64_t fpsN_;
    int64_t fpsD_;
    int64_t originNs_ = 0;
};

}  // namespace moo
