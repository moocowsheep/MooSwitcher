#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "core/Format.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"
#include "ndi/NdiLib.h"

namespace moo {

// NDI program output: waits for completed pack readbacks, hands the host
// buffer straight to NDIlib_send_send_video_async_v2 (the SDK SpeedHQ-encodes
// from our memory; a slot stays pinned until the *next* async call releases
// it). Drop-to-latest when the encoder falls behind. Video-only until M4.
class NdiOutput {
public:
    NdiOutput(std::string name, gpu::Compositor& comp, gpu::Timeline& readbackTL);
    ~NdiOutput();

    int64_t framesSent() const { return sent_.load(std::memory_order_relaxed); }

private:
    void run(std::stop_token st);

    std::string name_;
    gpu::Compositor& comp_;
    gpu::Timeline& readbackTL_;
    NDIlib_send_instance_t sender_ = nullptr;
    std::atomic<int64_t> sent_{0};
    std::jthread thread_;
};

}  // namespace moo
