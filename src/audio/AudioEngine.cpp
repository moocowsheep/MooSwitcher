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
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#include "audio/AudioEngine.h"

#include <algorithm>
#include <cmath>

#include "core/Stats.h"

namespace moo::audio {

void InputChannel::notePts(int64_t senderPtsNs) {
    // Estimated playout of this chunk's first sample: everything already in
    // the ring plays first at the mixer's fixed drain rate. Measured before
    // our own delay line, so applying the auto trim never feeds back into
    // the measurement.
    const int64_t now = MediaClock::nowNs();
    const int64_t aheadFrames = int64_t(ring_.fill()) / kChannels;
    const int64_t playout = now + aheadFrames * 1'000'000'000LL / kSampleRate;
    const int64_t s = playout - senderPtsNs;
    const int64_t prev = pmp_.load(std::memory_order_relaxed);
    const bool first = pmpAtNs_.load(std::memory_order_relaxed) == 0;
    pmp_.store(first ? s : prev + (s - prev) / 64, std::memory_order_relaxed);
    pmpAtNs_.store(now, std::memory_order_relaxed);
}

void InputChannel::pushPlanar(const float* l, const float* r, int frames,
                              int rate, int64_t senderPtsNs) {
    if (frames <= 0) return;
    if (rate != kSampleRate) {
        badRateFrames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    if (senderPtsNs != kNoPts) notePts(senderPtsNs);
    conv_.resize(size_t(frames) * kChannels);
    for (int f = 0; f < frames; ++f) {
        conv_[size_t(2 * f)] = l[f];
        conv_[size_t(2 * f + 1)] = r[f];
    }
    const size_t want = size_t(frames) * kChannels;
    const size_t got = ring_.write(conv_.data(), want);
    pushedFrames.fetch_add(frames, std::memory_order_relaxed);
    if (got < want)
        droppedFrames.fetch_add(int64_t(want - got) / kChannels,
                                std::memory_order_relaxed);
}

void InputChannel::pushInterleaved(const float* lr, int frames, int rate,
                                   int64_t senderPtsNs) {
    if (frames <= 0) return;
    if (rate != kSampleRate) {
        badRateFrames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    if (senderPtsNs != kNoPts) notePts(senderPtsNs);
    const size_t want = size_t(frames) * kChannels;
    const size_t got = ring_.write(lr, want);
    pushedFrames.fetch_add(frames, std::memory_order_relaxed);
    if (got < want)
        droppedFrames.fetch_add(int64_t(want - got) / kChannels,
                                std::memory_order_relaxed);
}

AudioEngine::AudioEngine(int nInputs)
    : core_(nInputs),
      mixSnap_(pack(0, 1, 0.f, 0.f)),
      dskSnap_(packDsk(-1, 0.f, -1, 0.f)) {
    for (int i = 0; i < nInputs; ++i)
        channels_.push_back(std::make_unique<InputChannel>());
}

AudioEngine::~AudioEngine() { stop(); }

void AudioEngine::addSink(PcmSink s) { sinks_.push_back(std::move(s)); }

void AudioEngine::start(int64_t clockOriginNs) {
    clk_.startAt(clockOriginNs);
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

void AudioEngine::stop() { thread_ = {}; }

int64_t AudioEngine::underruns() const {
    int64_t u = 0;
    for (const auto& c : channels_) u += c->underruns.load(std::memory_order_relaxed);
    return u;
}

uint64_t AudioEngine::pack(int pgm, int pvw, float alpha, float ftb) {
    auto q16 = [](float v) {
        return uint64_t(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f));
    };
    return (uint64_t(uint16_t(pgm)) << 48) | (uint64_t(uint16_t(pvw)) << 32) |
           (q16(alpha) << 16) | q16(ftb);
}

MixSnapshot AudioEngine::unpack(uint64_t v) {
    MixSnapshot s;
    s.pgm = int(uint16_t(v >> 48));
    s.pvw = int(uint16_t(v >> 32));
    s.alpha = float((v >> 16) & 0xFFFF) / 65535.f;
    s.ftb = float(v & 0xFFFF) / 65535.f;
    return s;
}

// Same layout per keyer as pack(): src as int16 (-1 = not following),
// on-screen level q16.
uint64_t AudioEngine::packDsk(int src0, float gain0, int src1, float gain1) {
    auto q16 = [](float v) {
        return uint64_t(std::lround(std::clamp(v, 0.f, 1.f) * 65535.f));
    };
    return (uint64_t(uint16_t(int16_t(src0))) << 48) | (q16(gain0) << 32) |
           (uint64_t(uint16_t(int16_t(src1))) << 16) | q16(gain1);
}

void AudioEngine::unpackDsk(uint64_t v, MixSnapshot& s) {
    s.dskSrc[0] = int(int16_t(uint16_t(v >> 48)));
    s.dskGain[0] = float((v >> 32) & 0xFFFF) / 65535.f;
    s.dskSrc[1] = int(int16_t(uint16_t(v >> 16)));
    s.dskGain[1] = float(v & 0xFFFF) / 65535.f;
}

void AudioEngine::run(std::stop_token st) {
    auto& tickCtr = Stats::counter("audio.ticks");
    auto& skipCtr = Stats::counter("audio.skips");

    const int n = inputCount();
    std::vector<std::vector<float>> bufs(
        size_t(n), std::vector<float>(size_t(kChunkFrames) * kChannels));
    std::vector<const float*> in(size_t(n), nullptr);
    std::vector<MixerCore::ChannelParams> params(static_cast<size_t>(n));
    // Frame-sync auto trim, applied value per channel. Jumps to the target
    // while the lane is held/re-arming (connect, replace -- silent anyway)
    // or on a generation bump; slews ~1 ms/s with a 0.5 ms deadband while
    // live, so the DelayLine tap never steps audibly.
    std::vector<int> autoApplied(size_t(n), 0);
    std::vector<uint32_t> autoGen(size_t(n), 0);
    std::vector<char> autoEver(size_t(n), 0);  // first target since (re)gen jumps
    std::vector<float> out(size_t(kChunkFrames) * kChannels);
    std::vector<float> peaks(size_t(n) * 2);
    std::vector<float> discard;
    float masterPeak[2] = {0.f, 0.f};

    auto maxStore = [](std::atomic<float>& a, float v) {
        if (v > a.load(std::memory_order_relaxed))
            a.store(v, std::memory_order_relaxed);
    };

    int64_t m = clk_.currentTick();
    while (!st.stop_requested()) {
        clk_.sleepUntilTick(m);

        MixSnapshot snap = unpack(mixSnap_.load(std::memory_order_relaxed));
        unpackDsk(dskSnap_.load(std::memory_order_relaxed), snap);

        for (int i = 0; i < n; ++i) {
            auto& ch = *channels_[size_t(i)];
            auto& prm = params[size_t(i)];
            prm.gain = ch.gain.load(std::memory_order_relaxed);
            prm.mute = ch.mute.load(std::memory_order_relaxed);
            prm.solo = ch.solo.load(std::memory_order_relaxed);

            const int target = ch.autoDelayFrames.load(std::memory_order_relaxed);
            int& ap = autoApplied[size_t(i)];
            if (const uint32_t g = ch.autoDelayGen.load(std::memory_order_relaxed);
                g != autoGen[size_t(i)]) {
                autoGen[size_t(i)] = g;
                autoEver[size_t(i)] = 0;
                ap = 0;
            }
            if (ch.holding_ || (!autoEver[size_t(i)] && target > 0)) {
                ap = target;  // lane silent / first lock: free to jump
            } else if (std::abs(target - ap) > framesForMs(1) / 2 && (m & 3) == 0) {
                ap += target > ap ? 1 : -1;  // 1 frame / 4 chunks ~= 1 ms/s
            }
            if (target > 0) autoEver[size_t(i)] = 1;
            prm.delayFrames = std::clamp(
                framesForMs(std::clamp(ch.delayMs.load(std::memory_order_relaxed),
                                       0, kMaxInputDelayMs)) +
                    ap,
                0, framesForMs(kMaxInputDelayMs));

            // Latency guard: after a stall or a connect backlog the ring can
            // sit far above prefill; drop back down so steady-state latency
            // stays bounded instead of pinning at ring capacity.
            size_t fill = ch.ring_.fill();
            if (fill > size_t(kMaxFillFrames) * kChannels) {
                const size_t excess = fill - size_t(kPrefillFrames) * kChannels;
                discard.resize(excess);
                const size_t got = ch.ring_.read(discard.data(), excess);
                ch.trimmedFrames.fetch_add(int64_t(got) / kChannels,
                                           std::memory_order_relaxed);
                fill = ch.ring_.fill();
            }

            if (ch.holding_ && fill >= size_t(kPrefillFrames) * kChannels) {
                if (ch.syncManaged.load(std::memory_order_relaxed) &&
                    fill > size_t(kPrefillFrames) * kChannels) {
                    // Drop the connect/reconnect backlog above prefill (see
                    // syncManaged in the header). Lane is silent: inaudible.
                    const size_t excess = fill - size_t(kPrefillFrames) * kChannels;
                    discard.resize(excess);
                    const size_t got = ch.ring_.read(discard.data(), excess);
                    ch.trimmedFrames.fetch_add(int64_t(got) / kChannels,
                                               std::memory_order_relaxed);
                }
                ch.holding_ = false;
            }

            if (!ch.holding_) {
                auto& buf = bufs[size_t(i)];
                const size_t got = ch.ring_.read(buf.data(), buf.size());
                if (got < buf.size()) {
                    std::fill(buf.begin() + long(got), buf.end(), 0.f);
                    ch.underruns.fetch_add(1, std::memory_order_relaxed);
                    ch.holding_ = true;  // re-arm to prefill
                }
                in[size_t(i)] = buf.data();
            } else {
                in[size_t(i)] = nullptr;
            }
        }

        const int mdel = framesForMs(std::clamp(
            masterDelayMs.load(std::memory_order_relaxed), 0, kMaxMasterDelayMs));
        core_.process(in.data(), params.data(), snap, mdel, out.data(),
                      peaks.data(), masterPeak);

        for (int i = 0; i < n; ++i) {
            maxStore(channels_[size_t(i)]->peakL, peaks[size_t(i) * 2]);
            maxStore(channels_[size_t(i)]->peakR, peaks[size_t(i) * 2 + 1]);
        }
        maxStore(masterPeakL, masterPeak[0]);
        maxStore(masterPeakR, masterPeak[1]);

        for (auto& s : sinks_) s(out.data(), kChunkFrames, m * kChunkFrames);

        ticks_.fetch_add(1, std::memory_order_relaxed);
        tickCtr.add();

        // Fell behind (scheduler stall): resync to the clock. The sample
        // counter jumps with m, so downstream PTS stay on the timeline and
        // the gap is an audible dropout, not creeping desync.
        int64_t next = m + 1;
        const int64_t cur = clk_.currentTick();
        if (cur > next) {
            skips_.fetch_add(cur - next, std::memory_order_relaxed);
            skipCtr.add(cur - next);
            next = cur;
        }
        m = next;
    }
}

}  // namespace moo::audio
