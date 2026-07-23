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
#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace moo {

// Per-input frame synchronizer (docs/design-framesync.md). Re-times a
// source's frames onto the render tick grid at a target delay of syncFrames
// source periods past each frame's earliest plausible arrival, absorbing
// decoder emission bursts and turning rate slip into counted, deterministic
// repeats/drops instead of a per-session phase lottery. Pure logic: no
// threads, no clocks, no I/O. The render thread owns one instance per
// sync-enabled input; the frame handle is a template parameter so tests
// drive the scheduler with plain ints.
//
// pts -> local-monotonic mapping: anchor = min over the last kWindow frames
// of (arrNs - srcPtsNs). Queueing and bursts only ever make that difference
// larger, so the window minimum tracks the least-delayed path offset (the
// classic dejitter estimator); pure sender-timestamp slew (NTP discipline)
// is absorbed by the anchor and never disturbs the schedule. The applied
// anchor follows the window minimum through a per-tick slew clamp so
// ppm-scale drift cannot step presentation; discontinuities re-anchor hard.
template <typename Frame>
class FrameSync {
public:
    static constexpr int kWindow = 128;                 // anchor min window
    static constexpr int64_t kSlewPerTickNs = 100'000;  // 0.1 ms per tick
    static constexpr int64_t kPtsGapNs = 500'000'000;   // resync threshold
    static constexpr uint64_t kSeqGap = 32;             // resync threshold
    static constexpr int kLockPresents = 8;             // locked() threshold

    struct Config {
        int64_t framePeriodNs = 0;  // Ts: source cadence
        int syncFrames = 1;         // N: delay target N*Ts (0 = measure-only)
    };

    struct Counters {
        int64_t starves = 0;      // repeated: nothing queued
        int64_t waits = 0;        // repeated: queued but not due yet (healthy)
        int64_t lateUploads = 0;  // a due frame's upload was still in flight
        int64_t slipDrops = 0;    // older due frames dropped (slip/burst catch-up)
        int64_t overflows = 0;    // queue cap hit, oldest dropped
        int64_t resyncs = 0;      // discontinuity re-anchors
    };

    explicit FrameSync(const Config& c) : cfg_(c) {}

    // Capture-side sample, in arrival order (the render thread drains the
    // input's feed ring and pushes here before calling present()).
    // senderClock: pts came from the sender/stream clock (false = receiver
    // synthesized them from arrival; the audio auto-trim must not compare
    // across that domain switch).
    void push(Frame f, uint64_t seq, int64_t srcPtsNs, int64_t arrNs,
              bool senderClock = true) {
        senderClock_ = senderClock;
        if (havePrev_) {
            const int64_t dPts = srcPtsNs - lastPtsNs_;
            if (seq - lastSeq_ > kSeqGap || dPts <= 0 || dPts > kPtsGapNs)
                resync();
        }
        lastSeq_ = seq;
        lastPtsNs_ = srcPtsNs;
        havePrev_ = true;

        const int64_t delta = arrNs - srcPtsNs;
        const uint64_t idx = pushIdx_++;
        while (!wedge_.empty() && wedge_.front().first + kWindow <= idx)
            wedge_.pop_front();
        while (!wedge_.empty() && wedge_.back().second >= delta)
            wedge_.pop_back();
        wedge_.emplace_back(idx, delta);
        anchorTarget_ = wedge_.front().second;
        if (!anchorValid_) {
            anchorApplied_ = anchorTarget_;
            anchorValid_ = true;
        }

        q_.push_back(Entry{std::move(f), srcPtsNs});
        const size_t cap = size_t(cfg_.syncFrames) + 4;
        while (q_.size() > cap) {
            q_.pop_front();
            ++ctr_.overflows;
        }
    }

    // Newest due frame at tick time tNs whose upload has completed, or
    // nullopt (caller repeats its previous frame). uploaded(const Frame&)
    // is re-checked every tick: a due frame still mid-DMA stays queued.
    template <typename UploadedFn>
    std::optional<Frame> present(int64_t tNs, UploadedFn&& uploaded) {
        if (anchorValid_) {
            if (presents_ < kLockPresents) {
                // Startup/resync: the seed frame may be mid-burst (SRT
                // connect backlog), leaving the anchor frames too high --
                // jump to the window minimum instead of crawling down at
                // slew rate for seconds.
                anchorApplied_ = anchorTarget_;
            } else {
                anchorApplied_ += std::clamp(anchorTarget_ - anchorApplied_,
                                             -kSlewPerTickNs, kSlewPerTickNs);
            }
        }
        const int64_t lag =
            anchorApplied_ + int64_t(cfg_.syncFrames) * cfg_.framePeriodNs;

        int dueEnd = 0;
        while (dueEnd < int(q_.size()) && q_[size_t(dueEnd)].ptsNs + lag <= tNs)
            ++dueEnd;
        if (dueEnd == 0) {
            if (q_.empty())
                ++ctr_.starves;
            else
                ++ctr_.waits;
            return std::nullopt;
        }
        int take = -1;  // newest due entry that has finished uploading
        for (int k = dueEnd - 1; k >= 0; --k) {
            if (uploaded(q_[size_t(k)].frame)) {
                take = k;
                break;
            }
        }
        if (take < 0) {
            ++ctr_.lateUploads;  // everything due is still in flight
            return std::nullopt;
        }
        if (take < dueEnd - 1) ++ctr_.lateUploads;
        ctr_.slipDrops += take;
        Entry e = std::move(q_[size_t(take)]);
        q_.erase(q_.begin(), q_.begin() + take + 1);

        const int64_t d = tNs - (e.ptsNs + anchorApplied_);
        const int64_t pmp = tNs - e.ptsNs;  // un-anchored, for the A/V trim
        if (!dVidValid_) {
            dVidNs_ = d;
            pmpNs_ = pmp;
            dVidValid_ = true;
        } else {
            dVidNs_ += (d - dVidNs_) / 64;
            pmpNs_ += (pmp - pmpNs_) / 64;
        }
        ++presents_;
        return std::move(e.frame);
    }

    const Counters& counters() const { return ctr_; }
    int depth() const { return int(q_.size()); }
    const Config& config() const { return cfg_; }

    // EWMA of the realized presentation delay (tick time minus anchored
    // pts): target delay + tick-phase remainder + upload margin. This is
    // the per-session number the audio auto-trim mirrors.
    int64_t videoDelayNs() const { return dVidValid_ ? dVidNs_ : 0; }
    // EWMA of (present tick time - raw pts), no anchor. Subtracting the
    // audio lane's (playout - pts) EWMA cancels the sender-clock terms and
    // yields the auto A/V trim directly (docs/design-framesync.md 3.4).
    int64_t presentMinusPtsNs() const { return dVidValid_ ? pmpNs_ : 0; }
    bool senderClock() const { return senderClock_; }
    // Enough presents since the last (re)anchor for videoDelayNs to mean
    // something.
    bool locked() const { return presents_ >= kLockPresents; }
    int64_t anchorNs() const { return anchorApplied_; }

private:
    struct Entry {
        Frame frame;
        int64_t ptsNs;
    };

    void resync() {
        q_.clear();
        wedge_.clear();
        anchorValid_ = false;
        dVidValid_ = false;
        presents_ = 0;
        ++ctr_.resyncs;
    }

    Config cfg_;
    std::deque<Entry> q_;
    std::deque<std::pair<uint64_t, int64_t>> wedge_;  // (push idx, arr-pts)
    uint64_t pushIdx_ = 0;
    int64_t anchorTarget_ = 0;
    int64_t anchorApplied_ = 0;
    bool anchorValid_ = false;
    bool havePrev_ = false;
    uint64_t lastSeq_ = 0;
    int64_t lastPtsNs_ = 0;
    int64_t dVidNs_ = 0;
    int64_t pmpNs_ = 0;
    bool dVidValid_ = false;
    bool senderClock_ = true;
    int64_t presents_ = 0;
    Counters ctr_;
};

}  // namespace moo
