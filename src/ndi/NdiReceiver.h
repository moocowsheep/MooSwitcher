#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "core/Spsc.h"
#include "gpu/UploadRing.h"
#include "ndi/NdiFinder.h"
#include "ndi/NdiLib.h"

namespace moo {

// One NDI input: capture thread doing recv_capture_v3 (UYVY fastest path),
// one memcpy into pinned staging, upload submit on the transfer queue, and a
// latest-frame mailbox publish. Handles source (re)connect by name substring
// and mid-show format changes (new UploadRing; the old one dies with its
// last published frame).
class NdiReceiver {
public:
    using FramePtr = std::shared_ptr<const gpu::GpuFrame>;
    using Mailbox = LatestMailbox<FramePtr>;

    NdiReceiver(gpu::VkEngine& eng, gpu::Queue& uploadQueue, NdiFinder& finder,
                std::string matchName, int index);
    ~NdiReceiver();

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const {
        return mailbox_.takeNewer(lastSeq);
    }

    struct Status {
        bool connected = false;
        int64_t frames = 0;
        int64_t drops = 0;
        VideoFormatDesc desc{};
    };
    Status status() const;
    const std::string& matchName() const { return match_; }

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

    std::atomic<bool> connected_{false};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> drops_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
