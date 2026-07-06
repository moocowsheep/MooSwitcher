#pragma once
#include <cstdint>
#include <vector>

namespace moo::audio {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;       // stereo bus in v1
// 5 ms mixer tick. Simulated worst-phase ring fill against NDI's ~16.7 ms
// audio burst cadence: prefill 960 + chunk 240 never dips below 566 frames
// (chunk 480: 328), and the finer grid halves mean chunk-quantization
// latency -- measured ~2.5 ms off both output paths' A/V centers.
constexpr int kChunkFrames = 240;
constexpr int kMaxInputDelayMs = 500;
constexpr int kMaxMasterDelayMs = 200;

constexpr int framesForMs(int ms) { return ms * (kSampleRate / 1000); }

// Latest bus/transition state from the video switcher; audio follows video.
// alpha: 0 = program only, 1 = preview fully on air. ftb: 0 = normal, 1 = black.
struct MixSnapshot {
    int pgm = 0;
    int pvw = 1;
    float alpha = 0.f;
    float ftb = 0.f;
};

// Interleaved-stereo delay line. The tap is chosen per chunk; changing it
// jumps the read position (acceptable for a config-rate control).
class DelayLine {
public:
    explicit DelayLine(int maxDelayFrames);
    void process(const float* in, float* out, int frames, int delayFrames);

private:
    std::vector<float> buf_;  // interleaved, power-of-two frame count
    size_t maskF_ = 0;
    uint64_t w_ = 0;
};

// Pure chunk-at-a-time mixer DSP: per-input delay trim + fader/mute/solo,
// video-follow equal-power crossfade + FTB dip, limiter, master delay.
// No threads, no clocks, no I/O -- everything here is unit-testable math.
// Gains ramp linearly across each chunk from their previous values, so
// 60 Hz control updates cannot produce zipper noise at 100 Hz chunks.
class MixerCore {
public:
    struct ChannelParams {
        float gain = 1.f;     // linear fader gain (mute applied on top)
        bool mute = false;
        bool solo = false;    // any solo: soloed inputs play, buses bypassed
        int delayFrames = 0;  // positive per-input trim
    };

    explicit MixerCore(int nInputs, int chunkFrames = kChunkFrames);

    // in[i]: chunkFrames*kChannels interleaved samples or nullptr (silence;
    // the input's delay-line tail still plays out). inPeak: 2*nInputs
    // post-fader chunk peaks (pre-bus, so meters read even off-air).
    // masterPeak: 2 floats, post-limiter.
    void process(const float* const* in, const ChannelParams* p,
                 const MixSnapshot& snap, int masterDelayFrames, float* out,
                 float* inPeak, float* masterPeak);

    int inputCount() const { return nIn_; }
    int chunkFrames() const { return chunk_; }

private:
    int nIn_;
    int chunk_;
    std::vector<DelayLine> inDelay_;
    DelayLine masterDelay_;
    std::vector<float> prevFader_, prevBus_;
    float prevFtbGain_ = 1.f;
    float limEnv_ = 0.f;
    std::vector<float> tmp_, mix_, zero_;
};

}  // namespace moo::audio
