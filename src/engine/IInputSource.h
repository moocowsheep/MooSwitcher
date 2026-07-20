/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <memory>
#include <optional>
#include <string>

#include "core/Format.h"
#include "core/Spsc.h"
#include "gpu/UploadRing.h"

namespace moo {

namespace audio {
class InputChannel;
}

// A switcher input: publishes latest-frame GpuFrames from a live transport,
// local clip decoder, or persistent still-image loader.
class IInputSource {
public:
    using FramePtr = std::shared_ptr<const gpu::GpuFrame>;
    using Mailbox = LatestMailbox<FramePtr>;

    // Frame-sync feed sample (docs/design-framesync.md): every publish,
    // timestamped. srcPtsNs is the sender/pts clock (deltas trustworthy,
    // absolutes not); arrNs is local CLOCK_MONOTONIC at capture.
    struct TimedFrame {
        FramePtr frame;
        uint64_t seq = 0;
        int64_t srcPtsNs = 0;
        int64_t arrNs = 0;
        bool senderClock = true;  // false: pts synthesized from arrival
    };
    using SyncFeed = SpscRing<TimedFrame>;

    struct Status {
        bool connected = false;
        int64_t frames = 0;
        int64_t drops = 0;
        VideoFormatDesc desc{};
    };

    struct MediaState {
        bool available = false;
        bool playing = false;
        bool loop = true;
        bool atEnd = false;
        int64_t positionMs = 0;
        int64_t durationMs = 0;
        int playlistIndex = 0;
        int playlistSize = 0;
        int64_t trimInMs = 0;
        int64_t trimOutMs = 0;
        int speedPermille = 1000;
        std::string currentRef;
    };

    virtual ~IInputSource() = default;
    virtual std::optional<Mailbox::Item> newer(uint64_t lastSeq) const = 0;

    // Up to Mailbox::kKeep retained publishes newer than lastSeq (newest
    // first) into out; returns the count. Lets the render tick fall back a
    // publish or two when the newest upload hasn't completed yet.
    virtual int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const {
        if (auto i = newer(lastSeq)) {
            out[0] = *i;
            return 1;
        }
        return 0;
    }

    virtual Status status() const = 0;
    virtual void setTally(bool /*onProgram*/, bool /*onPreview*/) {}
    // Static sources publish once and retain that frame indefinitely; the
    // render loop must not replace it with the no-signal placeholder after
    // the live-source stale timeout.
    virtual bool frameIsPersistent() const { return false; }
    virtual MediaState mediaState() const { return {}; }
    virtual void setMediaPlaying(bool /*playing*/) {}
    virtual void setMediaLoop(bool /*loop*/) {}
    virtual void restartMedia() {}
    virtual void stepMedia(int /*direction*/) {}

    // Non-null when the input was created with frame sync enabled: the
    // capture thread pushes every publish here too; the render tick drains
    // it into the input's FrameSync. The mailbox stays authoritative for
    // sync-off inputs.
    virtual SyncFeed* syncFeed() { return nullptr; }

    // Wire this input's audio into a mixer lane. Safe to call while running
    // (audio before attach is dropped); sources without audio ignore it.
    virtual void attachAudioSink(audio::InputChannel*) {}
};

}  // namespace moo
