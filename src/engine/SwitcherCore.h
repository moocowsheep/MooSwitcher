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

#pragma once
#include <cstdint>

namespace moo {

enum class TransitionType : uint8_t {
    Mix = 0, WipeLR, WipeRL, WipeTB, WipeBT, WipeBox, WipeCircle
};

inline constexpr int kDskCount = 2;

// Everything the compositor needs to render one output frame.
struct CompositeJob {
    int programSrc = 0;
    int previewSrc = 0;
    float alpha = 0.f;  // 0 = program only, 1 = preview fully on air
    TransitionType trans = TransitionType::Mix;
    float softness = 0.f;
    float ftb = 0.f;  // 0 = normal, 1 = full black
    bool transitionActive = false;
    // Downstream keyers, composited over the A/B mix (DSK2 over DSK1),
    // under FTB. level animates; on = engaged target (for tally).
    int dskSrc[kDskCount] = {0, 0};
    float dskLevel[kDskCount] = {0.f, 0.f};
    bool dskOn[kDskCount] = {false, false};
    bool dskTie[kDskCount] = {false, false};
    bool dskAudioFollow[kDskCount] = {false, false};
    // State after the next transition completes (look-ahead preview): a tied
    // keyer toggles, an untied one keeps its engaged target.
    bool dskFutureOn[kDskCount] = {false, false};
};

// Pure program/preview bus + transition state machine. No I/O, no clocks:
// callers apply control-surface commands between ticks and feed tick indices.
// Fully deterministic, so every behavior is pinned by unit tests.
//
// Semantics (hardware-switcher conventions):
//  - cut() during an active transition completes it instantly (swap).
//  - autoTransition() runs alpha 0->1 over the configured duration, then swaps.
//  - The T-bar is manual: position IS alpha; releasing below 1.0 holds the mix;
//    reaching 1.0 completes (swap) and re-arms at 0.
//  - Bus assigns cancel an active transition without swapping.
//  - FTB ramps toward its target over its own duration and is independent of
//    transitions (a post-program stage), rate-correct across skipped ticks.
class SwitcherCore {
public:
    // -- control surface (apply between ticks) --
    void setProgram(int src);
    void setPreview(int src);
    void setTransition(TransitionType t, int64_t durationTicks, float softness);
    void cut();
    void autoTransition(int64_t nowTick);
    void tbarBegin();
    void tbarSet(float pos);  // clamped to [0,1]; >= 1 completes + re-arms
    void tbarEnd();
    void fadeToBlack();  // toggles FTB target
    void setFtbDuration(int64_t ticks);
    // DSKs: independent on/off fades (FTB semantics) by default.
    // Source/duration changes never disturb a running level. A keyer with
    // tie set rides the NEXT transition instead (ATEM "next transition"
    // convention): auto/T-bar progress drives its level toward the toggled
    // state, cut snaps it, and a canceled transition ramps it back.
    void dskToggle(int k);
    void setDskSource(int k, int src);
    void setDskDuration(int k, int64_t ticks);
    void setDskTie(int k, bool tie);
    void setDskAudioFollow(int k, bool follow);

    CompositeJob tick(int64_t nowTick);

    int program() const { return program_; }
    int preview() const { return preview_; }
    TransitionType transitionType() const { return transType_; }
    int64_t transitionDuration() const { return transDur_; }
    float transitionSoftness() const { return softness_; }
    bool inTransition() const { return autoActive_ || tbarActive_; }
    bool ftbEngaged() const { return ftbTarget_ > 0.5f; }
    bool dskOn(int k) const { return dsk_[k].target > 0.5f; }
    float dskLevel(int k) const { return dsk_[k].level; }
    int dskSource(int k) const { return dsk_[k].src; }
    int64_t dskDuration(int k) const { return dsk_[k].dur; }
    bool dskTie(int k) const { return dsk_[k].tie; }
    bool dskAudioFollow(int k) const { return dsk_[k].audioFollow; }
    // Keyer state after the next transition lands (look-ahead preview).
    bool dskWillBeOn(int k) const {
        const Dsk& d = dsk_[k];
        if (d.tieRun) return d.target > 0.5f;  // riding this transition
        return d.tie ? !(d.target > 0.5f) : d.target > 0.5f;
    }

private:
    void completeTransition();
    void cancelTransition();
    void armTiedKeyers();

    int program_ = 0;
    int preview_ = 1;

    TransitionType transType_ = TransitionType::Mix;
    int64_t transDur_ = 30;
    float softness_ = 0.02f;

    bool autoActive_ = false;
    int64_t autoStart_ = 0;

    bool tbarActive_ = false;
    float tbarPos_ = 0.f;

    float ftbLevel_ = 0.f;
    float ftbTarget_ = 0.f;
    int64_t ftbDur_ = 30;

    struct Dsk {
        int src = 0;
        float level = 0.f;
        float target = 0.f;
        int64_t dur = 30;
        bool tie = false;          // keyer joins the next A/B transition
        bool audioFollow = false;  // mixer tracks level on the key source
        bool tieRun = false;       // level driven by the active transition
        float tieFrom = 0.f;       // level when that transition began
    };
    Dsk dsk_[kDskCount];

    int64_t lastTick_ = -1;  // shared by the FTB and DSK ramps
};

}  // namespace moo
