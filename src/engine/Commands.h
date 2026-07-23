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

#pragma once

namespace moo {

// GUI -> engine control commands, applied between ticks on the render thread.
struct Command {
    enum class Type {
        SetProgram,     // arg = source index
        SetPreview,     // arg = source index
        Cut,
        Auto,
        TbarBegin,
        TbarSet,        // farg = position 0..1
        TbarEnd,
        FadeToBlack,
        SetTransition,  // arg = TransitionType, arg2 = duration ticks, farg = softness
        DskToggle,      // arg = keyer index
        SetDskSource,   // arg = keyer index, arg2 = source index
        SetDskFade,     // arg = keyer index, arg2 = duration ticks
        SetDskTie,      // arg = keyer index, arg2 = bool
        SetDskAudioFollow,  // arg = keyer index, arg2 = bool
        MediaSetPlaying,  // arg = input index, arg2 = bool
        MediaRestart,     // arg = input index
        MediaSetLoop,     // arg = input index, arg2 = bool
        MediaStep,        // arg = input index, arg2 = -1 previous / +1 next
    } type = Type::Cut;
    int arg = 0;
    int arg2 = 0;
    float farg = 0.f;
};

}  // namespace moo
