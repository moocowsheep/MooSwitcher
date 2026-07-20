#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "audio/AudioEngine.h"
#include "audio/AudioRing.h"
#include "audio/MixerCore.h"

using namespace moo::audio;

namespace {

constexpr int kN = 480 * 2;  // one chunk, interleaved stereo

std::vector<float> dcChunk(float v) { return std::vector<float>(kN, v); }

std::vector<float> sineChunk(float amp, int periodFrames = 48) {
    std::vector<float> s(kN);
    for (int f = 0; f < 480; ++f) {
        const float v =
            amp * std::sin(2.f * float(M_PI) * float(f % periodFrames) /
                           float(periodFrames));
        s[size_t(2 * f)] = v;
        s[size_t(2 * f + 1)] = v;
    }
    return s;
}

struct Mix1 {
    std::vector<float> out = std::vector<float>(kN);
    std::vector<float> inPeak;
    float masterPeak[2] = {0, 0};

    Mix1(int n) : inPeak(size_t(n) * 2) {}
};

}  // namespace

TEST_CASE("audio ring: bulk write/read with wrap and drop accounting") {
    AudioRing r(1000);  // rounds up to 1024
    REQUIRE(r.capacity() == 1024);

    std::vector<float> src(700), dst(1024);
    for (int i = 0; i < 700; ++i) src[size_t(i)] = float(i);

    REQUIRE(r.write(src.data(), 700) == 700);
    REQUIRE(r.fill() == 700);
    REQUIRE(r.read(dst.data(), 500) == 500);
    for (int i = 0; i < 500; ++i) REQUIRE(dst[size_t(i)] == float(i));

    // Wraps across the end; only 824 slots free now.
    std::vector<float> big(900, 7.f);
    REQUIRE(r.write(big.data(), 900) == 824);
    REQUIRE(r.fill() == 1024);
    REQUIRE(r.write(big.data(), 10) == 0);  // full

    REQUIRE(r.read(dst.data(), 1024) == 1024);
    for (int i = 0; i < 200; ++i) REQUIRE(dst[size_t(i)] == float(500 + i));
    for (int i = 200; i < 1024; ++i) REQUIRE(dst[size_t(i)] == 7.f);
    REQUIRE(r.read(dst.data(), 16) == 0);  // empty
}

TEST_CASE("mixer: program passes through at unity after gain ramp-in") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    MixSnapshot snap{0, 1, 0.f, 0.f};

    const auto sig = sineChunk(0.5f);
    const float* in[2] = {sig.data(), nullptr};

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);

    for (int i = 0; i < kN; ++i)
        REQUIRE(std::fabs(m.out[size_t(i)] - sig[size_t(i)]) < 1e-5f);
    REQUIRE(std::fabs(m.inPeak[0] - 0.5f) < 1e-3f);
    REQUIRE(std::fabs(m.masterPeak[0] - 0.5f) < 1e-3f);
}

TEST_CASE("mixer: equal-power crossfade follows alpha; preview lands clean") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];

    const auto a = dcChunk(0.5f);
    const auto b = dcChunk(0.5f);
    const float* in[2] = {a.data(), b.data()};

    // Settle gains at alpha 0.
    MixSnapshot snap{0, 1, 0.f, 0.f};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-4f);

    // Mid-transition: both audible at the equal-power law.
    snap.alpha = 0.5f;
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    const float g = std::cos(0.25f * float(M_PI)) + std::sin(0.25f * float(M_PI));
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f * g) < 1e-3f);

    // Completed transition (video swaps buses, alpha back to 0).
    snap = {1, 0, 0.f, 0.f};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-4f);
}

TEST_CASE("mixer: same source on both buses holds level through transition") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    const auto sig = dcChunk(0.5f);
    const float* in[2] = {sig.data(), nullptr};

    MixSnapshot snap{0, 0, 0.f, 0.f};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    for (float alpha : {0.25f, 0.5f, 0.75f, 1.f}) {
        snap.alpha = alpha;
        core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
        REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-3f);
    }
}

TEST_CASE("mixer: mute silences program; meters go quiet too") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    p[0].mute = true;
    const auto sig = dcChunk(0.5f);
    const float* in[2] = {sig.data(), nullptr};
    MixSnapshot snap{0, 1, 0.f, 0.f};

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2]) < 1e-6f);
    REQUIRE(m.inPeak[0] < 1e-6f);
}

TEST_CASE("mixer: solo isolates even an off-air input") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    p[1].solo = true;  // input 1 is not on program
    const auto a = dcChunk(0.5f);
    const auto b = dcChunk(0.25f);
    const float* in[2] = {a.data(), b.data()};
    MixSnapshot snap{0, 1, 0.f, 0.f};

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.25f) < 1e-4f);
}

TEST_CASE("mixer: audio-follow-DSK lifts an off-air key source to its level") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    const auto b = dcChunk(0.5f);
    const float* in[2] = {nullptr, b.data()};  // input 1 off both buses
    MixSnapshot snap{0, 0, 0.f, 0.f};
    snap.dskSrc[0] = 1;

    snap.dskGain[0] = 0.5f;  // keyer half faded in
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.25f) < 1e-3f);

    snap.dskGain[0] = 1.f;  // fully keyed: unity
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-3f);

    snap.dskSrc[0] = -1;  // follow disengaged: back off air
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2]) < 1e-5f);
}

TEST_CASE("mixer: DSK follow max-combines with program, never doubles") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    const auto a = dcChunk(0.5f);
    const float* in[1] = {a.data()};
    MixSnapshot snap{0, 0, 0.f, 0.f};  // input 0 IS program...
    snap.dskSrc[0] = 0;                // ...and the followed key source
    snap.dskGain[0] = 1.f;

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-4f);  // unity, not 2x
}

TEST_CASE("mixer: solo bypasses DSK follow like the other buses") {
    MixerCore core(2, 480);
    Mix1 m(2);
    MixerCore::ChannelParams p[2];
    p[0].solo = true;
    const auto a = dcChunk(0.25f);
    const auto b = dcChunk(0.5f);
    const float* in[2] = {a.data(), b.data()};
    MixSnapshot snap{0, 0, 0.f, 0.f};
    snap.dskSrc[0] = 1;  // followed keyer at full...
    snap.dskGain[0] = 1.f;

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.25f) < 1e-4f);  // ...but solo wins
}

TEST_CASE("mixer: per-input delay shifts by the configured frames") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    p[0].delayFrames = 100;
    MixSnapshot snap{0, 0, 0.f, 0.f};

    const auto zeros = dcChunk(0.f);
    const float* in[1] = {zeros.data()};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);

    auto impulse = dcChunk(0.f);
    impulse[0] = impulse[1] = 0.5f;  // below the limiter ceiling
    in[0] = impulse.data();
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);

    for (int f = 0; f < 100; ++f) REQUIRE(std::fabs(m.out[size_t(2 * f)]) < 1e-6f);
    REQUIRE(std::fabs(m.out[200] - 0.5f) < 1e-4f);
    REQUIRE(std::fabs(m.out[201] - 0.5f) < 1e-4f);
    REQUIRE(std::fabs(m.out[202]) < 1e-6f);
}

TEST_CASE("mixer: master delay shifts the mixed output") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    MixSnapshot snap{0, 0, 0.f, 0.f};

    const auto zeros = dcChunk(0.f);
    const float* in[1] = {zeros.data()};
    core.process(in, p, snap, 48, m.out.data(), m.inPeak.data(), m.masterPeak);

    auto impulse = dcChunk(0.f);
    impulse[0] = impulse[1] = 0.5f;  // below the limiter ceiling
    in[0] = impulse.data();
    core.process(in, p, snap, 48, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[0]) < 1e-6f);
    REQUIRE(std::fabs(m.out[96] - 0.5f) < 1e-4f);
}

TEST_CASE("mixer: FTB dips to silence and back") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    const auto sig = dcChunk(0.5f);
    const float* in[1] = {sig.data()};

    MixSnapshot snap{0, 0, 0.f, 0.f};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);

    snap.ftb = 1.f;
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2]) < 1e-6f);
    REQUIRE(m.masterPeak[0] < 1e-6f);

    snap.ftb = 0.f;
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.5f) < 1e-4f);
}

TEST_CASE("mixer: limiter caps hot program at the ceiling, leaves quiet alone") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    MixSnapshot snap{0, 0, 0.f, 0.f};

    const auto hot = sineChunk(2.f);
    const float* in[1] = {hot.data()};
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    float peak = 0.f;
    for (int i = 0; i < kN; ++i) peak = std::max(peak, std::fabs(m.out[size_t(i)]));
    REQUIRE(peak <= 0.8913f + 1e-4f);
    REQUIRE(peak > 0.80f);  // limited, not crushed
    REQUIRE(m.inPeak[0] > 1.9f);  // input meter still shows the hot source

    MixerCore quietCore(1, 480);
    const auto quiet = sineChunk(0.3f);
    in[0] = quiet.data();
    quietCore.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    quietCore.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    for (int i = 0; i < kN; ++i)
        REQUIRE(std::fabs(m.out[size_t(i)] - quiet[size_t(i)]) < 1e-4f);
}

TEST_CASE("mixer: fader gain scales output and meters") {
    MixerCore core(1, 480);
    Mix1 m(1);
    MixerCore::ChannelParams p[1];
    p[0].gain = 0.25f;
    const auto sig = dcChunk(0.8f);
    const float* in[1] = {sig.data()};
    MixSnapshot snap{0, 0, 0.f, 0.f};

    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    core.process(in, p, snap, 0, m.out.data(), m.inPeak.data(), m.masterPeak);
    REQUIRE(std::fabs(m.out[kN - 2] - 0.2f) < 1e-4f);
    REQUIRE(std::fabs(m.inPeak[0] - 0.2f) < 1e-3f);
}

TEST_CASE("input channel: planar push interleaves, guards rate, counts drops") {
    InputChannel ch;

    std::vector<float> l(100), r(100);
    for (int i = 0; i < 100; ++i) {
        l[size_t(i)] = float(i);
        r[size_t(i)] = -float(i);
    }
    ch.pushPlanar(l.data(), r.data(), 100, 48000);
    REQUIRE(ch.pushedFrames.load() == 100);
    REQUIRE(ch.badRateFrames.load() == 0);

    ch.pushPlanar(l.data(), r.data(), 100, 44100);  // wrong rate: dropped
    REQUIRE(ch.badRateFrames.load() == 100);
    REQUIRE(ch.pushedFrames.load() == 100);

    // Mono callers duplicate the plane.
    ch.pushPlanar(l.data(), l.data(), 50, 48000);
    REQUIRE(ch.pushedFrames.load() == 150);
}

TEST_CASE("input channel: sync-managed lane drops connect backlog at hold release") {
    // A connect can land the transport's whole buffered backlog in the ring
    // in one push (SRT latency window). Sync-managed lanes must start playing
    // from exactly prefill or the parked excess becomes a per-session A/V
    // offset the auto trim cannot remove (negative demand clamps at 0).
    AudioEngine eng(2);
    eng.channel(0).syncManaged.store(true);   // sync lane: trims
    eng.channel(1).syncManaged.store(false);  // v1 lane: keeps backlog (G4)

    constexpr int kBacklog = 4000;  // > prefill 960, < the 4800 latency guard
    std::vector<float> lr(size_t(kBacklog) * kChannels, 0.1f);
    eng.channel(0).pushInterleaved(lr.data(), kBacklog, kSampleRate);
    eng.channel(1).pushInterleaved(lr.data(), kBacklog, kSampleRate);

    eng.publishMix(0, 1, 0.f, 0.f);
    eng.start(moo::MediaClock::nowNs());
    while (eng.mixTicks() < 4) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    eng.stop();

    REQUIRE(eng.channel(0).trimmedFrames.load() == kBacklog - kPrefillFrames);
    REQUIRE(eng.channel(1).trimmedFrames.load() == 0);
    REQUIRE(eng.channel(0).ringFillSamples() < eng.channel(1).ringFillSamples());
}

TEST_CASE("audio engine: sinks receive contiguous sample counters") {
    AudioEngine eng(1);
    std::vector<int64_t> firsts;
    std::vector<int> counts;
    eng.addSink([&](const float*, int frames, int64_t s0) {
        firsts.push_back(s0);
        counts.push_back(frames);
    });
    eng.publishMix(0, 1, 0.f, 0.f);
    eng.start(moo::MediaClock::nowNs());
    while (eng.mixTicks() < 8) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    eng.stop();

    REQUIRE(firsts.size() >= 8);
    for (size_t i = 0; i < firsts.size(); ++i)
        REQUIRE(counts[i] == kChunkFrames);
    if (eng.mixSkips() == 0)
        for (size_t i = 1; i < firsts.size(); ++i)
            REQUIRE(firsts[i] == firsts[i - 1] + kChunkFrames);
}
