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
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moo::ctl {

// Wire protocol for the TCP remote-control port (docs/remote-control.md).
// Requests are single text lines (case-insensitive keywords), so the surface
// works from Bitfocus Companion's generic TCP module or netcat as well as the
// bundled Companion module. State flows back as one-line JSON events.
//
// ALL input and DSK numbers on the wire are 1-based (matching the operator
// labels in the GUI); parse converts to the engine's 0-based indices.

struct Request {
    enum class Op {
        Cut,
        Auto,
        Ftb,
        SetProgram,   // a = input
        SetPreview,   // a = input
        SetTransition,  // a = TransitionType, b = duration ticks (0 = keep),
                        // f = softness (< 0 = keep)
        TbarBegin,
        TbarSet,      // f = position 0..1
        TbarEnd,
        DskSet,       // a = keyer, b = 0 off / 1 on / 2 toggle
        SetDskSource,  // a = keyer, b = input
        SetDskFade,    // a = keyer, b = duration ticks
        DskTie,        // a = keyer, b as DskSet (ride the next transition)
        DskAudioFollow,  // a = keyer, b as DskSet
        MediaPlay,     // a = input
        MediaPause,    // a = input
        MediaRestart,  // a = input
        MediaStep,     // a = input, b = -1 previous / +1 next
        MediaLoop,     // a = input, b = 0 off / 1 on
        RecordStart,   // s = path ("" = server picks a timestamped default)
        RecordStop,
        RecordToggle,  // s = path used when this starts a recording
        CleanRecordStart,
        CleanRecordStop,
        CleanRecordToggle,
        AudioMute,  // a = input (-1 = master n/a), b = 0 off / 1 on / 2 toggle
        AudioSolo,  // a = input, b as AudioMute
        AudioGain,  // a = input, f = linear gain 0..4
        Subscribe,
        Unsubscribe,
        GetState,
        Ping,
    } op = Op::Ping;
    int a = 0;
    int b = 0;
    float f = 0.f;
    std::string s;
};

// Parses one wire line (no trailing newline). Empty/comment lines return
// nullopt with err empty; bad input returns nullopt with err set.
std::optional<Request> parseLine(std::string_view line, std::string& err);

// TransitionType names accepted by TRANSITION and reported in state JSON,
// indexed by moo::TransitionType value.
const std::vector<std::string>& transitionNames();

// Engine state mirrored to clients. The server fills one per poll; a push
// goes out when the serialized form changes.
struct DskState {
    bool on = false;
    float level = 0.f;
    int src = 0;  // 0-based here; serialized 1-based
    bool tie = false;
    bool audioFollow = false;
};

struct MediaControlState {
    bool available = false;
    bool playing = false;
    bool loop = true;
    bool atEnd = false;
    int playlistIndex = 0;  // 0-based here; serialized 1-based
    int playlistSize = 0;
};

struct InputControlState {
    std::string ref;   // empty = unassigned (deliberate BLACK)
    int type = 0;      // InputSpec::Type as int
    bool connected = false;
    MediaControlState media;
    bool audioMute = false;
    bool audioSolo = false;
    float audioGain = 1.f;
};

struct RecordControlState {
    bool active = false;
    bool pending = false;
    bool error = false;
    int64_t frames = 0;
    std::string path;
};

struct Snapshot {
    int program = 0;  // 0-based here; serialized 1-based
    int preview = 1;
    double fps = 60000.0 / 1001.0;  // output rate (recording-time display)
    bool inTransition = false;
    bool ftb = false;
    float ftbLevel = 0.f;
    int transitionType = 0;
    std::vector<DskState> dsk;
    RecordControlState record;
    RecordControlState cleanRecord;
    bool srtConfigured = false;
    bool srtConnected = false;
    bool audioAvailable = false;
    std::vector<InputControlState> inputs;
};

// One-line {"event":"state",...} JSON (no trailing newline). Floats are
// fixed-precision so an idle engine serializes identically every poll.
std::string toJson(const Snapshot& s);

std::string jsonEscape(std::string_view s);

}  // namespace moo::ctl
