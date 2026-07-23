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

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include "common/pattern.h"

namespace pat = moo::pattern;

TEST_CASE("counter strips roundtrip through UYVY pixels") {
    const int W = 1280, H = 96;
    std::vector<uint8_t> frame(size_t(W) * H * 2, 0x80);

    for (uint64_t v : {0ULL, 1ULL, 0xFFFFFFFFFFFFFFFFULL, 0x123456789ABCDEF0ULL,
                       1234567890123ULL}) {
        pat::stampStrip(frame.data(), W * 2, pat::kCounterRow, v);
        uint64_t out = 0;
        REQUIRE(pat::readStrip(frame.data(), W * 2, pat::kCounterRow, out));
        REQUIRE(out == v);
    }
}

TEST_CASE("both strip rows are independent") {
    const int W = 1280, H = 96;
    std::vector<uint8_t> frame(size_t(W) * H * 2, 0x80);

    pat::stampStrip(frame.data(), W * 2, pat::kCounterRow, 42);
    pat::stampStrip(frame.data(), W * 2, pat::kTimeRow, 987654321098765ULL);

    uint64_t a = 0, b = 0;
    REQUIRE(pat::readStrip(frame.data(), W * 2, pat::kCounterRow, a));
    REQUIRE(pat::readStrip(frame.data(), W * 2, pat::kTimeRow, b));
    REQUIRE(a == 42);
    REQUIRE(b == 987654321098765ULL);
}

TEST_CASE("corrupted strip fails parity") {
    const int W = 1280, H = 96;
    std::vector<uint8_t> frame(size_t(W) * H * 2, 0x80);
    pat::stampStrip(frame.data(), W * 2, pat::kCounterRow, 0xDEADBEEF);

    // Flip one data block (block 3 -> luma inverted).
    pat::fillRectUYVY(frame.data(), W * 2, 3 * pat::kBlock, 0, pat::kBlock,
                      pat::kBlock, 235, 128, 128);
    uint64_t out = 0;
    const bool ok = pat::readStrip(frame.data(), W * 2, pat::kCounterRow, out);
    REQUIRE((!ok || out != 0xDEADBEEF));
}

TEST_CASE("flash region stamps and reads back") {
    const int W = 1280, H = 96;
    std::vector<uint8_t> frame(size_t(W) * H * 2, 0x80);
    pat::stampFlash(frame.data(), W * 2, true);
    REQUIRE(pat::readFlash(frame.data(), W * 2));
    pat::stampFlash(frame.data(), W * 2, false);
    REQUIRE_FALSE(pat::readFlash(frame.data(), W * 2));
}

TEST_CASE("sample/tick conversion is exact at broadcast rates") {
    // 60 ticks @ 60000/1001 = 1.001 s = 48048 samples exactly.
    REQUIRE(pat::sampleForTick(60, 60000, 1001) == 48048);
    REQUIRE(pat::sampleForTick(60, 60, 1) == 48000);
    REQUIRE(pat::sampleForTick(60, 30000, 1001) == 96096);
}

TEST_CASE("tone burst occupies exactly the first 100ms after each flash") {
    const int64_t period = 48048;
    REQUIRE(pat::toneSample(0, period) == 0.f);  // sin(0)
    REQUIRE(pat::toneSample(12, period) != 0.f);
    REQUIRE(pat::toneSample(pat::kToneBurstSamples - 1, period) != 0.f);
    REQUIRE(pat::toneSample(pat::kToneBurstSamples, period) == 0.f);
    REQUIRE(pat::toneSample(period - 1, period) == 0.f);
    REQUIRE(pat::toneSample(period + 12, period) != 0.f);
}
