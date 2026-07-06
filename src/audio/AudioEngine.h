#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "audio/AudioRing.h"
#include "audio/MixerCore.h"
#include "core/MediaClock.h"

namespace moo::audio {

constexpr int kRingFrames = 24000;    // 0.5 s of buffered source audio
constexpr int kPrefillFrames = 960;   // 20 ms arm level before a channel plays
constexpr int kMaxFillFrames = 4800;  // 100 ms: trim back to prefill beyond this

// One input's audio lane: an SPSC ring written by the capture/decode thread
// and drained by the mixer tick, plus operator controls and meters. The
// prefill hold absorbs sender chunk cadence (NDI delivers ~10-20 ms bursts);
// the trim bounds steady-state latency after stalls or connect backlogs.
class InputChannel {
public:
    InputChannel() : ring_(size_t(kRingFrames) * kChannels) {}

    // Writer side (one capture/decode thread per channel). Planar callers
    // with mono sources pass the same plane twice. Non-48k audio is counted
    // and dropped (NDI/SRT sources are 48 kHz in practice; v1 does not
    // resample on this path).
    void pushPlanar(const float* l, const float* r, int frames, int rate);
    void pushInterleaved(const float* lr, int frames, int rate);

    // Controls (any thread).
    std::atomic<float> gain{1.f};  // linear fader gain
    std::atomic<bool> mute{false};
    std::atomic<bool> solo{false};
    std::atomic<int> delayMs{0};  // 0..kMaxInputDelayMs positive trim

    // Meters: mixer stores chunk maxima; pollers exchange(0) to get
    // peak-since-last-poll. Counters are cumulative.
    std::atomic<float> peakL{0.f}, peakR{0.f};
    std::atomic<int64_t> pushedFrames{0}, droppedFrames{0};
    std::atomic<int64_t> underruns{0}, trimmedFrames{0}, badRateFrames{0};

    size_t ringFillSamples() const { return ring_.fill(); }

private:
    friend class AudioEngine;
    AudioRing ring_;
    std::vector<float> conv_;  // writer-thread interleave scratch
    bool holding_ = true;      // mixer-thread: wait for prefill before playing
};

// The audio mixer thread: 10 ms ticks on the SAME clock origin as the video
// render loop (sample index n*480 lines up with video tick time exactly), so
// output sample counts are valid PTS against video tick PTS. Each tick pulls
// the input rings, runs MixerCore with the latest video bus snapshot, updates
// meters, and fans the master chunk out to PCM sinks (NDI embed, AAC->TS).
// Sinks run on the mixer thread and must never block.
class AudioEngine {
public:
    // Sink receives the master mix: interleaved stereo, `frames` frames,
    // `firstSample` = absolute sample index since the clock origin.
    using PcmSink =
        std::function<void(const float* lr, int frames, int64_t firstSample)>;

    explicit AudioEngine(int nInputs);
    ~AudioEngine();

    InputChannel& channel(int i) { return *channels_[size_t(i)]; }
    int inputCount() const { return int(channels_.size()); }

    void addSink(PcmSink s);  // before start()
    void start(int64_t clockOriginNs);
    void stop();

    // Render thread, once per video tick: bus/transition state for the
    // audio-follow-video crossfade and FTB dip.
    void publishMix(int pgm, int pvw, float alpha, float ftb) {
        mixSnap_.store(pack(pgm, pvw, alpha, ftb), std::memory_order_relaxed);
    }

    // A/V calibration: master bus delay applied before fan-out.
    std::atomic<int> masterDelayMs{10};
    std::atomic<float> masterPeakL{0.f}, masterPeakR{0.f};

    int64_t mixTicks() const { return ticks_.load(std::memory_order_relaxed); }
    int64_t mixSkips() const { return skips_.load(std::memory_order_relaxed); }
    int64_t underruns() const;  // summed over inputs

private:
    void run(std::stop_token st);
    static uint64_t pack(int pgm, int pvw, float alpha, float ftb);
    static MixSnapshot unpack(uint64_t v);

    std::vector<std::unique_ptr<InputChannel>> channels_;
    MixerCore core_;
    MediaClock clk_{100, 1};  // 10 ms ticks, 480 samples each
    std::vector<PcmSink> sinks_;
    std::atomic<uint64_t> mixSnap_;
    std::atomic<int64_t> ticks_{0}, skips_{0};
    std::jthread thread_;
};

}  // namespace moo::audio
