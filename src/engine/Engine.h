#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
#include "media/Playlist.h"

namespace moo {

class NdiOutput;
class FileRecorder;
class SrtOutput;

struct InputSpec {
    enum class Type { Ndi, Srt, Omt, Media } type = Type::Ndi;
    std::string ref;  // NDI name, SRT/OMT URL, or local media path
    // Frame sync (docs/design-framesync.md): -1 = off (v1 latest-frame
    // behavior), 0 = measure-only (auto A/V trim, no added latency),
    // 1..4 = buffered re-timing by that many source frames.
    int syncFrames = -1;
    bool mediaPlaying = true;
    bool mediaLoop = true;
    // Ordered local clips. Empty means the legacy/single-clip `ref`.
    std::vector<media::PlaylistItem> mediaPlaylist;

    InputSpec() = default;
    InputSpec(Type inputType, std::string inputRef, int frames = -1)
        : type(inputType), ref(std::move(inputRef)), syncFrames(frames) {}
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
    int recordBitrateKbps = 0;  // 0 = auto; independent of SRT output
    bool audio = true;
    // A/V calibration. Measured on this box: with 0, offsets land at
    // 1080p NDI ~-1ms / SRT ~-7ms, 8K NDI ~+7ms (audio ages ~one capture
    // iteration more at 8K) -- all inside the +-10ms gate. Operators
    // re-trim per show from the GUI master strip.
    int masterAudioDelayMs = 0;
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

    // Source picker: swap input `index` to a new source. The render thread
    // performs the swap between ticks (placeholder until the new source
    // delivers); the old input is destroyed on a detached thread. SRT and
    // media specs require Vulkan/CUDA interop (initialized on demand; refused
    // if absent).
    void requestInputReplace(int index, InputSpec spec);
    std::vector<NdiFinder::Source> ndiSources() const;
    // OMT discovery snapshot (names in "HOST (Name)" form). Empty when built
    // without OMT. First call may return empty until mDNS answers arrive.
    std::vector<std::string> omtSources() const;
    std::string inputRef(int i) const;  // current source ref (UI labels)
    InputSpec::Type inputType(int i) const;
    int inputSyncFrames(int i) const;   // current frame-sync setting (-1 off)
    IInputSource::MediaState inputMediaState(int i) const;
    std::vector<media::PlaylistItem> inputMediaPlaylist(int i) const;

    struct RecordingState {
        bool active = false;
        bool pending = false;
        bool error = false;
        int64_t frames = 0;
        std::string path;
    };
    // Empty path stops recording. Encoder construction and finalization run
    // on the requesting thread; render/audio consumers switch atomically.
    void requestRecording(std::string path);
    RecordingState recordingState() const;

    bool copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w, int& h);

    using InputStatus = IInputSource::Status;

    struct UiState {
        int program = 0, preview = 1;
        bool inTransition = false;
        bool ftbEngaged = false;
        float ftbLevel = 0.f;
        bool dskOn[kDskCount] = {false, false};
        float dskLevel[kDskCount] = {0.f, 0.f};
        int dskSrc[kDskCount] = {0, 0};
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
    bool srtConfigured() const { return srtOut_ != nullptr; }
    VideoFormatDesc outputFormat() const { return cfg_.show; }

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
    // Atomic shared ownership lets the audio and render threads take a
    // non-blocking snapshot while start/stop swaps the active recorder.
    std::atomic<std::shared_ptr<FileRecorder>> recorder_;
    std::unique_ptr<audio::AudioEngine> audio_;
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> srtNvPushed_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> recordNvPushed_{};

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

    mutable std::mutex replaceM_;  // pending replaces + current input specs
    std::vector<std::pair<int, InputSpec>> pendingReplace_;

    mutable std::mutex recordM_;
    std::string requestedRecordingPath_;
    std::atomic<bool> recordingPending_{false};
    std::atomic<bool> recordingError_{false};
    std::atomic<uint64_t> recorderGeneration_{0};

    std::jthread renderThread_;
    std::atomic<bool> started_{false};
};

}  // namespace moo
