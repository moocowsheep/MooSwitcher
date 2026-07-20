/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
