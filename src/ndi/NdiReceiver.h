#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/Spsc.h"
#include "engine/IInputSource.h"
#include "gpu/UploadRing.h"
#include "ndi/NdiFinder.h"
#include "ndi/NdiLib.h"

namespace moo {

// One NDI input: capture thread doing recv_capture_v3 (UYVY fastest path),
// one memcpy into pinned staging, upload submit on the transfer queue, and a
// latest-frame mailbox publish. Handles source (re)connect by name substring
// and mid-show format changes (new UploadRing; the old one dies with its
// last published frame).
class NdiReceiver : public IInputSource {
public:
    NdiReceiver(gpu::VkEngine& eng, gpu::Queue& uploadQueue, NdiFinder& finder,
                std::string matchName, int index);
    ~NdiReceiver() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }

    Status status() const override;
    const std::string& matchName() const { return match_; }

    // Tally toward the source; applied on the capture thread (and re-applied
    // after reconnects).
    void setTally(bool onProgram, bool onPreview) override {
        tally_.store(uint8_t(onProgram | (onPreview << 1)),
                     std::memory_order_relaxed);
    }

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

private:
    void run(std::stop_token st);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    NdiFinder& finder_;
    std::string match_;       // lowercase substring
    std::string displayName_;
    int index_;

    NDIlib_recv_instance_t recv_ = nullptr;
    std::shared_ptr<gpu::UploadRing> ring_;
    Mailbox mailbox_;
    std::atomic<audio::InputChannel*> audioSink_{nullptr};

    std::atomic<bool> connected_{false};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> drops_{0};
    std::atomic<uint8_t> tally_{0};      // bit0 = program, bit1 = preview
    uint8_t appliedTally_ = 0xFF;        // capture-thread local
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
