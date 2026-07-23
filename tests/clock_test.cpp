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
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

#include <catch2/catch_test_macros.hpp>

#include "core/MediaClock.h"

using moo::MediaClock;

TEST_CASE("59.94 tick times are exact and drift-free") {
    MediaClock c(60000, 1001);
    c.startAt(0);

    // 60000 ticks of 1001/60000 s must land on exactly 1001 seconds.
    REQUIRE(c.nsForTick(60000) == 1001LL * 1'000'000'000);
    // And a full day of ticks stays exact (no accumulation error).
    const int64_t ticksPerDay = 60000LL * 86400 / 1001;  // floor
    const int64_t ns = c.nsForTick(ticksPerDay);
    // Independently: n * 1001e9 / 60000 with 128-bit math.
    const __int128 expect = (__int128(ticksPerDay) * 1001 * 1'000'000'000) / 60000;
    REQUIRE(ns == int64_t(expect));
}

TEST_CASE("tickForNs inverts nsForTick at boundaries") {
    MediaClock c(60000, 1001);
    c.startAt(123456789);

    for (int64_t n : {0LL, 1LL, 2LL, 59LL, 60LL, 59999LL, 60000LL, 60001LL,
                      1234567LL, 987654321LL}) {
        const int64_t t = c.nsForTick(n);
        REQUIRE(c.tickForNs(t) == n);
        REQUIRE(c.tickForNs(t - 1) == n - 1);
        REQUIRE(c.tickForNs(t + 1) == n);
    }
}

TEST_CASE("pre-origin times floor to negative ticks") {
    MediaClock c(60000, 1001);
    c.startAt(1'000'000'000);
    REQUIRE(c.tickForNs(999'999'999) < 0);
    REQUIRE(c.tickForNs(1'000'000'000) == 0);
}

TEST_CASE("integer rates work too") {
    MediaClock c(60, 1);
    c.startAt(0);
    REQUIRE(c.nsForTick(60) == 1'000'000'000);
    REQUIRE(c.nsForTick(1) == 16'666'666);  // truncated, consistent with inverse
    REQUIRE(c.tickForNs(16'666'666) == 1);
    REQUIRE(c.frameDurationNs() == 16'666'667);  // rounded display value
}

TEST_CASE("monotonic clock advances") {
    const int64_t a = MediaClock::nowNs();
    const int64_t b = MediaClock::nowNs();
    REQUIRE(b >= a);
}
