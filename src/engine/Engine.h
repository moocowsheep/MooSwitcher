#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio/AudioEngine.h"
#include "core/Format.h"
#include "core/MediaClock.h"
#include "core/Spsc.h"
#include "engine/Commands.h"
#include "engine/IInputSource.h"
#include "engine/SwitcherCore.h"
#include "gpu/Compositor.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"
#include "ndi/NdiFinder.h"
#include "ndi/NdiReceiver.h"

#include "media/CudaCtx.h"

namespace moo {

class NdiOutput;
class SrtOutput;

struct InputSpec {
    enum class Type { Ndi, Srt } type = Type::Ndi;
    std::string ref;  // NDI name substring, or srt:// URL
};

struct EngineConfig {
    VideoFormatDesc show{1920, 1080, 60000, 1001};
    int mvW = 1280;
    int mvH = 720;
    std::vector<InputSpec> inputs;
    bool validation = false;
    bool ndiOut = true;
    std::string ndiOutName = "MooSwitcher PGM";
    std::string srtUrl;      // empty = SRT output off
    int srtBitrateKbps = 0;  // 0 = auto
    bool audio = true;
    int masterAudioDelayMs = 10;  // A/V calibration; measured in M4
};

// Owns the GPU, the NDI inputs and program output, the switcher state
// machine, and the 59.94-paced render thread. Qt-free.
class Engine {
public:
    Engine();   // out-of-line: members hold unique_ptrs to fwd-declared types
    ~Engine();

    bool start(const EngineConfig& cfg);
    void stop();

    void post(const Command& c) { cmds_.push(c); }

    bool copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w, int& h);

    using InputStatus = IInputSource::Status;

    struct UiState {
        int program = 0, preview = 1;
        bool inTransition = false;
        bool ftbEngaged = false;
        float ftbLevel = 0.f;
    };
    UiState uiState() const {
        std::lock_guard lk(uiM_);
        return ui_;
    }

    int inputCount() const { return int(inputs_.size()); }
    InputStatus inputStatus(int i) const { return inputs_[i]->status(); }

    // Audio mixer controls/meters (null when cfg.audio == false). Channel
    // atomics and meters are safe to poke from the GUI thread.
    audio::AudioEngine* audio() { return audio_.get(); }
    const audio::AudioEngine* audio() const { return audio_.get(); }
    int64_t renderedTicks() const { return ticks_.load(std::memory_order_relaxed); }
    int64_t skippedTicks() const { return skips_.load(std::memory_order_relaxed); }
    int64_t ndiOutFrames() const;
    int64_t srtFramesEncoded() const;
    bool srtConnected() const;

private:
    void renderLoop(std::stop_token st);
    bool createPlaceholder();
    bool buildLabelAtlas();

    EngineConfig cfg_;
    gpu::VkEngine vk_;
    std::unique_ptr<gpu::Compositor> comp_;
    std::unique_ptr<NdiFinder> finder_;
    std::vector<std::unique_ptr<IInputSource>> inputs_;
    std::unique_ptr<NdiOutput> ndiOut_;
    media::CudaCtx cuda_;
    std::unique_ptr<SrtOutput> srtOut_;
    std::unique_ptr<audio::AudioEngine> audio_;
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> nvPushed_{};

    std::shared_ptr<gpu::UploadRing> placeholderRing_;
    std::shared_ptr<const gpu::GpuFrame> placeholder_;

    SpscRing<Command> cmds_{64};
    MediaClock clock_;
    SwitcherCore switcher_;  // render thread only

    gpu::Timeline renderTL_;
    gpu::Timeline readbackTL_;  // pack -> host copies (xferDown)
    VkCommandPool gfxPool_ = VK_NULL_HANDLE;
    VkCommandPool downPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, gpu::Compositor::kFramesInFlight> cmdBufs_{};
    std::array<VkCommandBuffer, gpu::Compositor::kFramesInFlight> downBufs_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> fifValues_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> downValues_{};
    std::array<std::vector<std::shared_ptr<const gpu::GpuFrame>>,
               gpu::Compositor::kFramesInFlight>
        retention_;

    std::atomic<uint64_t> mvReady_{0};
    std::array<std::atomic<uint64_t>, gpu::Compositor::kReadbackSlots> rbStamp_{};
    std::atomic<int64_t> ticks_{0}, skips_{0};

    mutable std::mutex uiM_;
    UiState ui_;

    std::jthread renderThread_;
    bool started_ = false;
};

}  // namespace moo
