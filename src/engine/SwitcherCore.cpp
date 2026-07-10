#include "engine/SwitcherCore.h"

#include <algorithm>
#include <utility>

namespace moo {

void SwitcherCore::setProgram(int src) {
    cancelTransition();
    program_ = src;
}

void SwitcherCore::setPreview(int src) {
    cancelTransition();
    preview_ = src;
}

void SwitcherCore::setTransition(TransitionType t, int64_t durationTicks, float softness) {
    transType_ = t;
    transDur_ = std::max<int64_t>(1, durationTicks);
    softness_ = std::clamp(softness, 0.f, 1.f);
}

void SwitcherCore::cut() { completeTransition(); }

void SwitcherCore::autoTransition(int64_t nowTick) {
    if (autoActive_ || tbarActive_) return;  // one transition at a time
    autoActive_ = true;
    autoStart_ = nowTick;
}

void SwitcherCore::tbarBegin() {
    if (autoActive_) return;  // auto owns the transition until it lands
    tbarActive_ = true;
}

void SwitcherCore::tbarSet(float pos) {
    if (!tbarActive_) return;
    tbarPos_ = std::clamp(pos, 0.f, 1.f);
    if (tbarPos_ >= 1.f) completeTransition();  // landed: swap + re-arm at 0
}

void SwitcherCore::tbarEnd() {
    // Releasing mid-travel holds the mix (honest T-bar); nothing to do.
}

void SwitcherCore::fadeToBlack() { ftbTarget_ = ftbTarget_ > 0.5f ? 0.f : 1.f; }

void SwitcherCore::setFtbDuration(int64_t ticks) { ftbDur_ = std::max<int64_t>(1, ticks); }

void SwitcherCore::dskToggle(int k) {
    if (k < 0 || k >= kDskCount) return;
    dsk_[k].target = dsk_[k].target > 0.5f ? 0.f : 1.f;
}

void SwitcherCore::setDskSource(int k, int src) {
    if (k < 0 || k >= kDskCount) return;
    dsk_[k].src = src;  // live change allowed; level untouched
}

void SwitcherCore::setDskDuration(int k, int64_t ticks) {
    if (k < 0 || k >= kDskCount) return;
    dsk_[k].dur = std::max<int64_t>(1, ticks);
}

void SwitcherCore::completeTransition() {
    std::swap(program_, preview_);
    autoActive_ = false;
    tbarActive_ = false;
    tbarPos_ = 0.f;
}

void SwitcherCore::cancelTransition() {
    autoActive_ = false;
    tbarActive_ = false;
    tbarPos_ = 0.f;
}

CompositeJob SwitcherCore::tick(int64_t nowTick) {
    // FTB and DSK levels ramp by elapsed ticks so skipped ticks stay
    // rate-correct.
    const int64_t elapsed =
        lastTick_ >= 0 && nowTick > lastTick_ ? nowTick - lastTick_ : 0;
    auto ramp = [elapsed](float& level, float target, int64_t dur) {
        if (!elapsed) return;
        const float step = float(elapsed) / float(dur);
        if (level < target) level = std::min(target, level + step);
        else if (level > target) level = std::max(target, level - step);
    };
    ramp(ftbLevel_, ftbTarget_, ftbDur_);
    for (auto& d : dsk_) ramp(d.level, d.target, d.dur);
    lastTick_ = nowTick;

    float alpha = 0.f;
    if (autoActive_) {
        alpha = float(nowTick - autoStart_ + 1) / float(transDur_);
        if (alpha >= 1.f) {
            completeTransition();  // this frame shows the landed state
            alpha = 0.f;
        }
    } else if (tbarActive_) {
        alpha = tbarPos_;
    }

    CompositeJob job;
    job.programSrc = program_;
    job.previewSrc = preview_;
    job.alpha = alpha;
    job.trans = transType_;
    job.softness = softness_;
    job.ftb = ftbLevel_;
    job.transitionActive = autoActive_ || tbarActive_;
    for (int k = 0; k < kDskCount; ++k) {
        job.dskSrc[k] = dsk_[k].src;
        job.dskLevel[k] = dsk_[k].level;
        job.dskOn[k] = dsk_[k].target > 0.5f;
    }
    return job;
}

}  // namespace moo
