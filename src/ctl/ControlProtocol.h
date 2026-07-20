/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
