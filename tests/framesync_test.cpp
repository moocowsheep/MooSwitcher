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

#include <cstdint>
#include <vector>

#include "engine/FrameSync.h"

using FS = moo::FrameSync<int>;

namespace {

constexpr int64_t kT = 16'683'333;  // 59.94 tick, ns (1001e9/60000 truncated)

struct SrcFrame {
    int64_t pts, arr;
};

struct SimOut {
    std::vector<int> shown;  // presented frame ids in tick order
    FS::Counters warm, end;  // counters at warmTicks and at the end
    int64_t dVid = 0;
    int64_t anchor = 0;
};

// Drive ticks 0..ticks-1 at period T; before each tick, push every source
// frame that has arrived (arr <= tick time), exactly like the render loop
// draining the feed ring. seq counts pushes (1-based), as the receivers do.
template <typename Up>
SimOut run(FS& fs, const std::vector<SrcFrame>& src, int64_t T, int ticks,
           int warmTicks, Up&& up) {
    SimOut o;
    size_t next = 0;
    for (int n = 0; n < ticks; ++n) {
        const int64_t t = int64_t(n) * T;
        while (next < src.size() && src[next].arr <= t) {
            fs.push(int(next), next + 1, src[next].pts, src[next].arr);
            ++next;
        }
        if (n == warmTicks) o.warm = fs.counters();
        if (auto f = fs.present(t, [&](const int& id) { return up(id, t); }))
            o.shown.push_back(*f);
    }
    o.end = fs.counters();
    o.dVid = fs.videoDelayNs();
    o.anchor = fs.anchorNs();
    return o;
}

SimOut run(FS& fs, const std::vector<SrcFrame>& src, int64_t T, int ticks,
           int warmTicks) {
    return run(fs, src, T, ticks, warmTicks,
               [](int, int64_t) { return true; });
}

std::vector<SrcFrame> cadence(int n, int64_t Ts, int64_t phase) {
    std::vector<SrcFrame> v;
    v.reserve(size_t(n));
    for (int k = 0; k < n; ++k)
        v.push_back({int64_t(k) * Ts, int64_t(k) * Ts + phase});
    return v;
}

bool consecutive(const std::vector<int>& v) {
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] != v[i - 1] + 1) return false;
    return !v.empty();
}

}  // namespace

TEST_CASE("clean cadence: phase sweep presents every tick, exact delay") {
    for (int p = 0; p < 16; ++p) {
        const int64_t phase = p * kT / 16;
        FS fs({kT, 1});
        const auto o = run(fs, cadence(700, kT, phase), kT, 600, 50);
        INFO("phase step " << p);
        REQUIRE(o.end.starves == o.warm.starves);
        REQUIRE(o.end.waits == o.warm.waits);
        REQUIRE(o.end.slipDrops == 0);
        REQUIRE(o.end.lateUploads == 0);
        REQUIRE(o.end.resyncs == 0);
        REQUIRE(consecutive(o.shown));
        // d = tickTime - (pts + anchor): N*T plus the tick-phase remainder.
        REQUIRE(o.dVid == kT + (kT - phase) % kT);
        REQUIRE(fs.locked());
    }
}

TEST_CASE("measure-only (N=0) adds no latency beyond tick quantization") {
    const int64_t phase = 5'000'000;
    FS fs({kT, 0});
    const auto o = run(fs, cadence(700, kT, phase), kT, 600, 50);
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.starves == o.warm.starves);
    REQUIRE(consecutive(o.shown));
    REQUIRE(o.dVid == kT - phase);  // first tick after arrival, v1 semantics
}

TEST_CASE("rate slip: 60.000 source on the 59.94 grid drops deterministically") {
    const int64_t Ts = 16'666'667;  // 60.000
    FS fs({Ts, 1});
    const auto o = run(fs, cadence(3700, Ts, 4'000'000), kT, 3600, 50);
    const int64_t drops = o.end.slipDrops - o.warm.slipDrops;
    // (ticks * (kT - Ts) / Ts) extra frames over 3550 ticks ~= 3.5
    REQUIRE(drops >= 2);
    REQUIRE(drops <= 5);
    REQUIRE(o.end.starves == o.warm.starves);
    REQUIRE(o.end.waits == o.warm.waits);
    REQUIRE(o.end.resyncs == 0);
}

TEST_CASE("rate slip: slow source repeats, never drops") {
    const int64_t Ts = 16'700'000;  // ~59.88
    FS fs({Ts, 1});
    const auto o = run(fs, cadence(3700, Ts, 4'000'000), kT, 3600, 50);
    const int64_t repeats = (o.end.starves + o.end.waits) -
                            (o.warm.starves + o.warm.waits);
    REQUIRE(repeats >= 2);
    REQUIRE(repeats <= 6);
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.resyncs == 0);
}

TEST_CASE("half-rate source presents every other tick") {
    const int64_t Ts = 2 * kT;  // 29.97 on the 59.94 grid
    FS fs({Ts, 1});
    const auto o = run(fs, cadence(400, Ts, 3'000'000), kT, 600, 50);
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.starves == o.warm.starves);
    const int64_t repeats = o.end.waits - o.warm.waits;
    REQUIRE(repeats >= 270);  // ~half of 550 post-warm ticks
    REQUIRE(repeats <= 280);
    REQUIRE(consecutive(o.shown));
}

namespace {
// Decoder-emission clump: groups of 4 frames all land when the last member
// of the group would have (pts cadence stays clean) -- the SRT stall shape.
std::vector<SrcFrame> clumped(int n, int64_t Ts, int64_t phase) {
    std::vector<SrcFrame> v;
    v.reserve(size_t(n));
    for (int k = 0; k < n; ++k) {
        const int64_t g = k / 4;
        v.push_back({int64_t(k) * Ts, (4 * g + 3) * Ts + phase});
    }
    return v;
}
}  // namespace

TEST_CASE("burst clumps: N=3 absorbs them completely") {
    FS fs({kT, 3});
    const auto o = run(fs, clumped(500, kT, 5'000'000), kT, 448, 48);
    REQUIRE(o.end.starves == o.warm.starves);
    REQUIRE(o.end.waits == o.warm.waits);
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.lateUploads == 0);
    REQUIRE(consecutive(o.shown));
}

TEST_CASE("burst clumps: N=1 degrades deterministically, stays ordered") {
    FS fs({kT, 1});
    const auto o = run(fs, clumped(500, kT, 5'000'000), kT, 448, 48);
    // Per 4-frame group: 2 presents, 2 empty ticks, 2 catch-up drops.
    REQUIRE(o.end.slipDrops - o.warm.slipDrops == 200);
    REQUIRE(o.end.starves - o.warm.starves == 200);
    for (size_t i = 1; i < o.shown.size(); ++i)
        REQUIRE(o.shown[i] > o.shown[i - 1]);
}

TEST_CASE("signal gap: starves during, one resync, clean relock after") {
    std::vector<SrcFrame> src = cadence(120, kT, 3'000'000);
    for (int k = 180; k < 400; ++k)  // source timeline kept running
        src.push_back({int64_t(k) * kT, int64_t(k) * kT + 3'000'000});
    FS fs({kT, 1});
    const auto o = run(fs, src, kT, 460, 20);
    REQUIRE(o.end.resyncs == 1);
    REQUIRE(o.end.starves - o.warm.starves >= 55);  // ~60 silent ticks
    REQUIRE(consecutive(o.shown));                  // ids stay in order
    REQUIRE(o.shown.back() >= 330);                 // presented past resume
    REQUIRE(fs.locked());
}

TEST_CASE("seq gap forces a resync and flushes older queued frames") {
    FS fs({kT, 1});
    fs.push(0, 1, 0, 1000);
    fs.push(1, 2, kT, kT + 1000);
    REQUIRE(fs.depth() == 2);
    fs.push(2, 40, 2 * kT, 2 * kT + 1000);  // 38-publish hole in the feed
    REQUIRE(fs.counters().resyncs == 1);
    REQUIRE(fs.depth() == 1);
}

TEST_CASE("non-advancing pts forces a resync") {
    FS fs({kT, 1});
    fs.push(0, 1, 5 * kT, 5 * kT + 1000);
    fs.push(1, 2, 5 * kT, 5 * kT + 2000);  // stuck sender timestamp
    REQUIRE(fs.counters().resyncs == 1);
}

TEST_CASE("sender timestamp slew is absorbed by the anchor") {
    // pts run 1000 ppm fast against a true Ts emission cadence -- 250x the
    // NTP slew measured in M5, still well inside the anchor slew clamp.
    const int64_t phase = 6'000'000;
    std::vector<SrcFrame> src;
    for (int k = 0; k < 2100; ++k)
        src.push_back({int64_t(k) * kT + int64_t(k) * kT / 1000,
                       int64_t(k) * kT + phase});
    FS fs({kT, 1});
    const auto o = run(fs, src, kT, 2000, 100);
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.starves == o.warm.starves);
    REQUIRE(o.end.resyncs == 0);
    REQUIRE(consecutive(o.shown));
    // The anchor tracked the drift (fell by ~ticks*T/1000)...
    const int64_t expectedFall = 2000LL * kT / 1000;
    REQUIRE(o.anchor < phase - expectedFall + 1'000'000);
    REQUIRE(o.anchor > phase - expectedFall - 1'000'000);
    // ...so the realized delay stayed put.
    const int64_t clean = kT + (kT - phase) % kT;
    REQUIRE(o.dVid > clean - 500'000);
    REQUIRE(o.dVid < clean + 500'000);
}

TEST_CASE("due frame mid-upload is deferred, never dropped") {
    // N=0, frames arrive 1 ms before each tick, uploads take 5 ms: every
    // tick defers the fresh frame and presents the previous one.
    std::vector<SrcFrame> src;
    for (int k = 0; k < 700; ++k) {
        const int64_t t = int64_t(k) * kT + kT - 1'000'000;
        src.push_back({t, t});
    }
    FS fs({kT, 0});
    const auto o = run(fs, src, kT, 600, 50, [&](int id, int64_t t) {
        return t >= src[size_t(id)].arr + 5'000'000;
    });
    REQUIRE(o.end.slipDrops == 0);
    REQUIRE(o.end.lateUploads - o.warm.lateUploads == 550);  // one per tick
    REQUIRE(consecutive(o.shown));
    REQUIRE(o.dVid == kT + 1'000'000);  // pushed out exactly one tick
}

TEST_CASE("queue overflow drops oldest and counts") {
    FS fs({kT, 1});  // cap = syncFrames + 4 = 5
    for (int k = 0; k < 10; ++k)
        fs.push(k, uint64_t(k) + 1, int64_t(k) * kT, 9 * kT);
    REQUIRE(fs.depth() == 5);
    REQUIRE(fs.counters().overflows == 5);
}
