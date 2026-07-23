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
    armTiedKeyers();
}

void SwitcherCore::tbarBegin() {
    // Auto owns the transition until it lands; a re-grab of a held T-bar
    // continues the same transition (arming tied keyers twice would flip
    // their destination back).
    if (autoActive_ || tbarActive_) return;
    tbarActive_ = true;
    armTiedKeyers();
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
    // While riding a transition the toggle redirects the destination; the
    // level keeps following the transition's progress.
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

void SwitcherCore::setDskTie(int k, bool tie) {
    if (k < 0 || k >= kDskCount) return;
    dsk_[k].tie = tie;
    // Untying mid-transition releases the keyer where it is; its own ramp
    // then finishes the move toward target.
    if (!tie) dsk_[k].tieRun = false;
}

void SwitcherCore::setDskAudioFollow(int k, bool follow) {
    if (k < 0 || k >= kDskCount) return;
    dsk_[k].audioFollow = follow;
}

void SwitcherCore::armTiedKeyers() {
    for (auto& d : dsk_) {
        if (!d.tie) continue;
        d.tieRun = true;
        d.tieFrom = d.level;
        d.target = d.target > 0.5f ? 0.f : 1.f;
    }
}

void SwitcherCore::completeTransition() {
    std::swap(program_, preview_);
    const bool wasActive = autoActive_ || tbarActive_;
    for (auto& d : dsk_) {
        if (d.tieRun) {
            d.level = d.target;  // land with the buses
            d.tieRun = false;
        } else if (d.tie && !wasActive) {
            // Cut with no running transition IS the next transition: tied
            // keyers snap-toggle with the bus swap.
            d.target = d.target > 0.5f ? 0.f : 1.f;
            d.level = d.target;
        }
    }
    autoActive_ = false;
    tbarActive_ = false;
    tbarPos_ = 0.f;
}

void SwitcherCore::cancelTransition() {
    for (auto& d : dsk_) {
        if (!d.tieRun) continue;
        d.tieRun = false;
        // Abandoned transition: head back to the pre-transition state at the
        // keyer's own fade rate.
        d.target = d.tieFrom > 0.5f ? 1.f : 0.f;
    }
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
    for (auto& d : dsk_)
        if (!d.tieRun) ramp(d.level, d.target, d.dur);
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
    // Tied keyers ride the transition's progress, not their own clock (a
    // completed transition above already parked them at target).
    if (autoActive_ || tbarActive_)
        for (auto& d : dsk_)
            if (d.tieRun) d.level = d.tieFrom + (d.target - d.tieFrom) * alpha;

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
        job.dskTie[k] = dsk_[k].tie;
        job.dskAudioFollow[k] = dsk_[k].audioFollow;
        job.dskFutureOn[k] = dskWillBeOn(k);
    }
    return job;
}

}  // namespace moo
