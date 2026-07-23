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

#include "ctl/ControlProtocol.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace moo::ctl {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

// Splits on runs of whitespace. `rest[i]` holds the untrimmed remainder of
// the line after token i — RECORD START paths may contain spaces.
struct Tokens {
    std::vector<std::string> tok;
    std::vector<std::string> rest;
};

Tokens tokenize(std::string_view line) {
    Tokens t;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        const size_t start = i;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
        t.tok.emplace_back(line.substr(start, i - start));
        size_t r = i;
        while (r < line.size() && std::isspace((unsigned char)line[r])) ++r;
        t.rest.emplace_back(line.substr(r));
    }
    return t;
}

bool parseInt(const std::string& s, int& out) {
    const char* b = s.data();
    const char* e = b + s.size();
    auto [p, ec] = std::from_chars(b, e, out);
    return ec == std::errc() && p == e;
}

bool parseFloat(const std::string& s, float& out) {
    try {
        size_t pos = 0;
        out = std::stof(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

// ON / OFF / TOGGLE -> 1 / 0 / 2.
bool parseOnOff(const std::string& word, int& out) {
    const std::string w = lower(word);
    if (w == "on") out = 1;
    else if (w == "off") out = 0;
    else if (w == "toggle") out = 2;
    else return false;
    return true;
}

// 1-based wire input -> 0-based engine index.
bool parseInput(const std::string& s, int& out) {
    if (!parseInt(s, out) || out < 1) return false;
    --out;
    return true;
}

std::optional<Request> fail(std::string& err, std::string msg) {
    err = std::move(msg);
    return std::nullopt;
}

}  // namespace

const std::vector<std::string>& transitionNames() {
    static const std::vector<std::string> names = {
        "mix", "wipelr", "wiperl", "wipetb", "wipebt", "wipebox", "wipecircle"};
    return names;
}

std::optional<Request> parseLine(std::string_view line, std::string& err) {
    err.clear();
    const Tokens t = tokenize(line);
    if (t.tok.empty() || t.tok[0][0] == '#') return std::nullopt;
    const std::string cmd = lower(t.tok[0]);
    const size_t n = t.tok.size();
    Request r;

    auto one = [&](Request::Op op) {
        r.op = op;
        return std::optional<Request>(r);
    };

    if (cmd == "cut" || cmd == "take") return one(Request::Op::Cut);
    if (cmd == "auto" || cmd == "trans") return one(Request::Op::Auto);
    if (cmd == "ftb") return one(Request::Op::Ftb);
    if (cmd == "subscribe") return one(Request::Op::Subscribe);
    if (cmd == "unsubscribe") return one(Request::Op::Unsubscribe);
    if (cmd == "state" || cmd == "get") return one(Request::Op::GetState);
    if (cmd == "ping") return one(Request::Op::Ping);

    if (cmd == "pgm" || cmd == "program" || cmd == "pvw" || cmd == "preview") {
        if (n != 2 || !parseInput(t.tok[1], r.a))
            return fail(err, cmd + " needs an input number (1-based)");
        r.op = (cmd == "pgm" || cmd == "program") ? Request::Op::SetProgram
                                                  : Request::Op::SetPreview;
        return r;
    }

    if (cmd == "transition") {
        if (n < 2) return fail(err, "transition needs a type name");
        const std::string name = lower(t.tok[1]);
        const auto& names = transitionNames();
        const auto it = std::find(names.begin(), names.end(), name);
        if (it == names.end())
            return fail(err, "unknown transition '" + name + "'");
        r.op = Request::Op::SetTransition;
        r.a = int(it - names.begin());
        r.b = 0;    // keep current duration
        r.f = -1.f;  // keep current softness
        if (n >= 3 && (!parseInt(t.tok[2], r.b) || r.b < 1 || r.b > 600))
            return fail(err, "transition duration is 1..600 frames");
        if (n >= 4 && (!parseFloat(t.tok[3], r.f) || r.f < 0.f || r.f > 1.f))
            return fail(err, "transition softness is 0..1");
        return r;
    }

    if (cmd == "tbar") {
        if (n != 2) return fail(err, "tbar needs BEGIN, END, or a 0..1 position");
        const std::string w = lower(t.tok[1]);
        if (w == "begin") return one(Request::Op::TbarBegin);
        if (w == "end") return one(Request::Op::TbarEnd);
        if (!parseFloat(t.tok[1], r.f) || r.f < 0.f || r.f > 1.f)
            return fail(err, "tbar position is 0..1");
        r.op = Request::Op::TbarSet;
        return r;
    }

    if (cmd == "dsk") {
        if (n < 3 || !parseInt(t.tok[1], r.a) || r.a < 1)
            return fail(err, "dsk needs a keyer number (1-based) and an action");
        --r.a;
        const std::string w = lower(t.tok[2]);
        if (parseOnOff(t.tok[2], r.b)) {
            r.op = Request::Op::DskSet;
            return r;
        }
        if (w == "src" || w == "source") {
            if (n != 4 || !parseInput(t.tok[3], r.b))
                return fail(err, "dsk src needs an input number (1-based)");
            r.op = Request::Op::SetDskSource;
            return r;
        }
        if (w == "fade") {
            if (n != 4 || !parseInt(t.tok[3], r.b) || r.b < 0 || r.b > 600)
                return fail(err, "dsk fade is 0..600 frames");
            r.op = Request::Op::SetDskFade;
            return r;
        }
        if (w == "tie" || w == "afv" || w == "follow") {
            const bool tie = w == "tie";
            r.b = 2;  // bare form toggles
            if (n == 4 && !parseOnOff(t.tok[3], r.b))
                return fail(err, std::string("dsk ") + (tie ? "tie" : "afv") +
                                     " takes ON/OFF/TOGGLE");
            if (n > 4)
                return fail(err, std::string("dsk ") + (tie ? "tie" : "afv") +
                                     " takes ON/OFF/TOGGLE");
            r.op = tie ? Request::Op::DskTie : Request::Op::DskAudioFollow;
            return r;
        }
        return fail(err, "dsk action is ON/OFF/TOGGLE/SRC/FADE/TIE/AFV");
    }

    if (cmd == "media") {
        if (n < 3 || !parseInput(t.tok[1], r.a))
            return fail(err, "media needs an input number (1-based) and an action");
        const std::string w = lower(t.tok[2]);
        if (w == "play") return one(Request::Op::MediaPlay);
        if (w == "pause") return one(Request::Op::MediaPause);
        if (w == "restart") return one(Request::Op::MediaRestart);
        if (w == "next") { r.b = 1; return one(Request::Op::MediaStep); }
        if (w == "prev" || w == "previous") {
            r.b = -1;
            return one(Request::Op::MediaStep);
        }
        if (w == "loop") {
            if (n != 4 || !parseOnOff(t.tok[3], r.b) || r.b == 2)
                return fail(err, "media loop needs ON or OFF");
            r.op = Request::Op::MediaLoop;
            return r;
        }
        return fail(err, "media action is PLAY/PAUSE/RESTART/NEXT/PREV/LOOP");
    }

    if (cmd == "record" || cmd == "clean") {
        const bool clean = cmd == "clean";
        if (n < 2) return fail(err, cmd + " needs START/STOP/TOGGLE");
        const std::string w = lower(t.tok[1]);
        if (w == "start" || w == "toggle") {
            r.op = clean ? (w == "start" ? Request::Op::CleanRecordStart
                                         : Request::Op::CleanRecordToggle)
                         : (w == "start" ? Request::Op::RecordStart
                                         : Request::Op::RecordToggle);
            if (n >= 3) r.s = t.rest[1];  // optional path, may contain spaces
            return r;
        }
        if (w == "stop") {
            r.op = clean ? Request::Op::CleanRecordStop : Request::Op::RecordStop;
            return r;
        }
        return fail(err, cmd + " action is START/STOP/TOGGLE");
    }

    if (cmd == "audio") {
        if (n < 3 || !parseInput(t.tok[1], r.a))
            return fail(err, "audio needs an input number (1-based) and an action");
        const std::string w = lower(t.tok[2]);
        if (w == "mute" || w == "solo") {
            if (n != 4 || !parseOnOff(t.tok[3], r.b))
                return fail(err, "audio " + w + " needs ON/OFF/TOGGLE");
            r.op = w == "mute" ? Request::Op::AudioMute : Request::Op::AudioSolo;
            return r;
        }
        if (w == "gain") {
            if (n != 4 || !parseFloat(t.tok[3], r.f) || r.f < 0.f || r.f > 4.f)
                return fail(err, "audio gain is linear 0..4");
            r.op = Request::Op::AudioGain;
            return r;
        }
        return fail(err, "audio action is MUTE/SOLO/GAIN");
    }

    return fail(err, "unknown command '" + cmd + "'");
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

namespace {

void appendF3(std::string& out, float v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.3f", double(v));
    out += buf;
}

void appendBool(std::string& out, const char* key, bool v) {
    out += '"';
    out += key;
    out += v ? "\":true" : "\":false";
}

void appendRecord(std::string& out, const char* key,
                  const RecordControlState& r) {
    out += '"';
    out += key;
    out += "\":{";
    appendBool(out, "active", r.active);
    out += ',';
    appendBool(out, "pending", r.pending);
    out += ',';
    appendBool(out, "error", r.error);
    out += ",\"frames\":" + std::to_string(r.frames);
    out += ",\"path\":\"" + jsonEscape(r.path) + "\"}";
}

}  // namespace

std::string toJson(const Snapshot& s) {
    std::string out;
    out.reserve(512 + s.inputs.size() * 160);
    out += "{\"event\":\"state\"";
    out += ",\"program\":" + std::to_string(s.program + 1);
    out += ",\"preview\":" + std::to_string(s.preview + 1);
    out += ",\"fps\":";
    appendF3(out, float(s.fps));
    out += ',';
    appendBool(out, "inTransition", s.inTransition);
    out += ',';
    appendBool(out, "ftb", s.ftb);
    out += ",\"ftbLevel\":";
    appendF3(out, s.ftbLevel);
    const auto& tn = transitionNames();
    out += ",\"transition\":\"";
    out += size_t(s.transitionType) < tn.size() ? tn[size_t(s.transitionType)]
                                                : "mix";
    out += '"';
    out += ",\"dsk\":[";
    for (size_t k = 0; k < s.dsk.size(); ++k) {
        if (k) out += ',';
        out += "{";
        appendBool(out, "on", s.dsk[k].on);
        out += ",\"level\":";
        appendF3(out, s.dsk[k].level);
        out += ",\"src\":" + std::to_string(s.dsk[k].src + 1);
        out += ',';
        appendBool(out, "tie", s.dsk[k].tie);
        out += ',';
        appendBool(out, "afv", s.dsk[k].audioFollow);
        out += '}';
    }
    out += ']';
    out += ',';
    appendRecord(out, "record", s.record);
    out += ',';
    appendRecord(out, "cleanRecord", s.cleanRecord);
    out += ",\"srt\":{";
    appendBool(out, "configured", s.srtConfigured);
    out += ',';
    appendBool(out, "connected", s.srtConnected);
    out += '}';
    out += ',';
    appendBool(out, "audio", s.audioAvailable);
    out += ",\"inputs\":[";
    for (size_t i = 0; i < s.inputs.size(); ++i) {
        const auto& in = s.inputs[i];
        if (i) out += ',';
        out += "{\"n\":" + std::to_string(i + 1);
        out += ",\"ref\":\"" + jsonEscape(in.ref) + "\"";
        out += ",\"type\":" + std::to_string(in.type);
        out += ',';
        appendBool(out, "connected", in.connected);
        if (in.media.available) {
            out += ",\"media\":{";
            appendBool(out, "playing", in.media.playing);
            out += ',';
            appendBool(out, "loop", in.media.loop);
            out += ',';
            appendBool(out, "atEnd", in.media.atEnd);
            out += ",\"item\":" + std::to_string(in.media.playlistIndex + 1);
            out += ",\"items\":" + std::to_string(in.media.playlistSize) + "}";
        }
        if (s.audioAvailable) {
            out += ',';
            appendBool(out, "mute", in.audioMute);
            out += ',';
            appendBool(out, "solo", in.audioSolo);
            out += ",\"gain\":";
            appendF3(out, in.audioGain);
        }
        out += '}';
    }
    out += "]}";
    return out;
}

}  // namespace moo::ctl
