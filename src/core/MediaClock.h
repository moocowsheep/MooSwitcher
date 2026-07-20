/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
