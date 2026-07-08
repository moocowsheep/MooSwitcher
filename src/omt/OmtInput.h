#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/Spsc.h"
#include "engine/IInputSource.h"
#include "gpu/UploadRing.h"

typedef long long omt_receive_t;  // matches libomt.h; keep it out of headers

namespace moo {

// One OMT (Open Media Transport) input: capture thread doing omt_receive
// (UYVY preferred format -- libomt decodes VMX on the fly), one memcpy into
// pinned staging, upload submit on the transfer queue, and a latest-frame
// mailbox publish. `ref` is either a discovery name ("HOST (Name)") or a
// direct URL ("omt://host:port"); the library reconnects internally, and a
// 3 s video gap recreates the receiver as a belt-and-braces fallback.
class OmtInput : public IInputSource {
public:
    // syncFrames >= 0 enables the frame-sync feed (and sizes the upload
    // ring for that many queued frames); -1 = plain latest-frame input.
    OmtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, std::string ref,
             int index, int syncFrames = -1);
    ~OmtInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }

    Status status() const override;
    const std::string& ref() const { return ref_; }

    void setTally(bool onProgram, bool onPreview) override {
        tally_.store(uint8_t(onProgram | (onPreview << 1)),
                     std::memory_order_relaxed);
    }

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

    SyncFeed* syncFeed() override { return syncFrames_ >= 0 ? &feed_ : nullptr; }

private:
    void run(std::stop_token st);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    std::string ref_;
    int index_;

    omt_receive_t* recv_ = nullptr;  // capture-thread owned after ctor
    std::shared_ptr<gpu::UploadRing> ring_;
    Mailbox mailbox_;
    const int syncFrames_;
    SyncFeed feed_{16};
    std::atomic<audio::InputChannel*> audioSink_{nullptr};

    std::atomic<bool> connected_{false};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> drops_{0};
    std::atomic<uint8_t> tally_{0};  // bit0 = program, bit1 = preview
    uint8_t appliedTally_ = 0xFF;    // capture-thread local
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
