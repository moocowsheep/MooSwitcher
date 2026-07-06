#include "audio/MixerCore.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace moo::audio {

namespace {
constexpr float kLimThreshold = 0.8913f;  // -1 dBFS ceiling
const float kLimRelease = std::exp(-1.f / (0.080f * kSampleRate));  // 80 ms
}  // namespace

DelayLine::DelayLine(int maxDelayFrames) {
    size_t cap = 1;
    while (cap < size_t(maxDelayFrames) + 1) cap <<= 1;
    maskF_ = cap - 1;
    buf_.assign(cap * kChannels, 0.f);
}

void DelayLine::process(const float* in, float* out, int frames,
                        int delayFrames) {
    for (int f = 0; f < frames; ++f) {
        const size_t wi = size_t(w_ & maskF_) * kChannels;
        buf_[wi] = in[2 * f];
        buf_[wi + 1] = in[2 * f + 1];
        const size_t ri = size_t((w_ - uint64_t(delayFrames)) & maskF_) * kChannels;
        out[2 * f] = buf_[ri];
        out[2 * f + 1] = buf_[ri + 1];
        ++w_;
    }
}

MixerCore::MixerCore(int nInputs, int chunkFrames)
    : nIn_(nInputs),
      chunk_(chunkFrames),
      masterDelay_(framesForMs(kMaxMasterDelayMs)),
      prevFader_(size_t(nInputs), 0.f),
      prevBus_(size_t(nInputs), 0.f) {
    for (int i = 0; i < nInputs; ++i)
        inDelay_.emplace_back(framesForMs(kMaxInputDelayMs));
    tmp_.resize(size_t(chunk_) * kChannels);
    mix_.resize(size_t(chunk_) * kChannels);
    zero_.assign(size_t(chunk_) * kChannels, 0.f);
}

void MixerCore::process(const float* const* in, const ChannelParams* p,
                        const MixSnapshot& snap, int masterDelayFrames,
                        float* out, float* inPeak, float* masterPeak) {
    std::fill(mix_.begin(), mix_.end(), 0.f);

    bool anySolo = false;
    for (int i = 0; i < nIn_; ++i) anySolo |= p[i].solo;

    constexpr float kHalfPi = std::numbers::pi_v<float> / 2.f;
    const float a = std::clamp(snap.alpha, 0.f, 1.f);
    const float invChunk = 1.f / float(chunk_);

    for (int i = 0; i < nIn_; ++i) {
        float bus;
        if (anySolo)
            bus = p[i].solo ? 1.f : 0.f;  // PFL-style: solo bypasses the buses
        else if (i == snap.pgm && i == snap.pvw)
            bus = 1.f;  // same source on both buses: no mid-transition bump
        else if (i == snap.pgm)
            bus = std::cos(a * kHalfPi);
        else if (i == snap.pvw)
            bus = std::sin(a * kHalfPi);
        else
            bus = 0.f;

        const float f1 = p[i].mute ? 0.f : p[i].gain;
        const float f0 = prevFader_[size_t(i)];
        const float b1 = bus;
        const float b0 = prevBus_[size_t(i)];
        prevFader_[size_t(i)] = f1;
        prevBus_[size_t(i)] = b1;

        const float* src = in[i] ? in[i] : zero_.data();
        inDelay_[size_t(i)].process(src, tmp_.data(), chunk_,
                                    std::clamp(p[i].delayFrames, 0,
                                               framesForMs(kMaxInputDelayMs)));

        float pl = 0.f, pr = 0.f;
        for (int f = 0; f < chunk_; ++f) {
            const float t = float(f + 1) * invChunk;
            const float gf = f0 + (f1 - f0) * t;
            const float gb = b0 + (b1 - b0) * t;
            const float l = tmp_[size_t(2 * f)] * gf;
            const float r = tmp_[size_t(2 * f + 1)] * gf;
            pl = std::max(pl, std::fabs(l));
            pr = std::max(pr, std::fabs(r));
            mix_[size_t(2 * f)] += l * gb;
            mix_[size_t(2 * f + 1)] += r * gb;
        }
        inPeak[2 * i] = pl;
        inPeak[2 * i + 1] = pr;
    }

    // Master: FTB dip -> limiter -> delay (the A/V-sync trim).
    const float ftb1 = 1.f - std::clamp(snap.ftb, 0.f, 1.f);
    const float ftb0 = prevFtbGain_;
    prevFtbGain_ = ftb1;

    float mpl = 0.f, mpr = 0.f;
    for (int f = 0; f < chunk_; ++f) {
        const float t = float(f + 1) * invChunk;
        const float g = ftb0 + (ftb1 - ftb0) * t;
        float l = mix_[size_t(2 * f)] * g;
        float r = mix_[size_t(2 * f + 1)] * g;
        const float peak = std::max(std::fabs(l), std::fabs(r));
        limEnv_ = std::max(peak, limEnv_ * kLimRelease);
        if (limEnv_ > kLimThreshold) {
            const float lg = kLimThreshold / limEnv_;
            l *= lg;
            r *= lg;
        }
        mpl = std::max(mpl, std::fabs(l));
        mpr = std::max(mpr, std::fabs(r));
        mix_[size_t(2 * f)] = l;
        mix_[size_t(2 * f + 1)] = r;
    }
    masterPeak[0] = mpl;
    masterPeak[1] = mpr;

    masterDelay_.process(mix_.data(), out, chunk_,
                         std::clamp(masterDelayFrames, 0,
                                    framesForMs(kMaxMasterDelayMs)));
}

}  // namespace moo::audio
