#pragma once
#include <memory>
#include <optional>

#include "core/Format.h"
#include "core/Spsc.h"
#include "gpu/UploadRing.h"

namespace moo {

namespace audio {
class InputChannel;
}

// A switcher input: publishes latest-frame GpuFrames from some transport
// (NDI receiver, SRT/NVDEC decoder, later stills/players).
class IInputSource {
public:
    using FramePtr = std::shared_ptr<const gpu::GpuFrame>;
    using Mailbox = LatestMailbox<FramePtr>;

    struct Status {
        bool connected = false;
        int64_t frames = 0;
        int64_t drops = 0;
        VideoFormatDesc desc{};
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

    // Wire this input's audio into a mixer lane. Safe to call while running
    // (audio before attach is dropped); sources without audio ignore it.
    virtual void attachAudioSink(audio::InputChannel*) {}
};

}  // namespace moo
