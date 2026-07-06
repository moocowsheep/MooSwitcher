#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Format.h"
#include "core/MediaClock.h"
#include "core/Spsc.h"
#include "engine/Commands.h"
#include "engine/SwitcherCore.h"
#include "gpu/Compositor.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"
#include "ndi/NdiFinder.h"
#include "ndi/NdiReceiver.h"

namespace moo {

struct EngineConfig {
    VideoFormatDesc show{1920, 1080, 60000, 1001};
    int mvW = 1280;
    int mvH = 720;
    std::vector<std::string> inputs;  // NDI source name substrings
    bool validation = false;
};

// Owns the GPU, the NDI inputs, the switcher state machine, and the
// 59.94-paced render thread. Qt-free; the GUI talks through post() and
// copyMultiview().
class Engine {
public:
    ~Engine() { stop(); }

    bool start(const EngineConfig& cfg);
    void stop();

    void post(const Command& c) { cmds_.push(c); }

    // Copies the newest completed multiview frame if newer than lastSeq.
    bool copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w, int& h);

    int inputCount() const { return int(inputs_.size()); }
    NdiReceiver::Status inputStatus(int i) const { return inputs_[i]->status(); }
    int64_t renderedTicks() const { return ticks_.load(std::memory_order_relaxed); }
    int64_t skippedTicks() const { return skips_.load(std::memory_order_relaxed); }

private:
    void renderLoop(std::stop_token st);
    bool createPlaceholder();

    EngineConfig cfg_;
    gpu::VkEngine vk_;
    std::unique_ptr<gpu::Compositor> comp_;
    std::unique_ptr<NdiFinder> finder_;
    std::vector<std::unique_ptr<NdiReceiver>> inputs_;

    std::shared_ptr<gpu::UploadRing> placeholderRing_;
    std::shared_ptr<const gpu::GpuFrame> placeholder_;

    SpscRing<Command> cmds_{64};
    MediaClock clock_;
    SwitcherCore switcher_;  // render thread only

    gpu::Timeline renderTL_;
    VkCommandPool gfxPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, gpu::Compositor::kFramesInFlight> cmdBufs_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> fifValues_{};
    std::array<std::vector<std::shared_ptr<const gpu::GpuFrame>>,
               gpu::Compositor::kFramesInFlight>
        retention_;

    std::atomic<uint64_t> mvReady_{0};
    std::array<std::atomic<uint64_t>, gpu::Compositor::kReadbackSlots> rbStamp_{};
    std::atomic<int64_t> ticks_{0}, skips_{0};

    std::jthread renderThread_;
    bool started_ = false;
};

}  // namespace moo
