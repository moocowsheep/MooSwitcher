#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "core/Spsc.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"
#include "media/FfmpegNvenc.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace moo {

struct SrtOutConfig {
    std::string url;         // srt://host:port?mode=...&latency=<usec>...
    int bitrateKbps = 0;     // 0 = auto from resolution/fps
};

// SRT/HEVC program output. The render thread packs NV12 into per-FIF
// exportable buffers and pushes {value, tick, fif} events; the encode thread
// waits the render timeline, bridges the buffer into an FFmpeg CUDA hwframe
// (copiedValue() is the render-side reuse handshake), and encodes; the mux
// thread owns the MPEG-TS/SRT connection with reconnect. A dead or slow SRT
// peer can never block the render clock: events and packets are dropped.
class SrtOutput {
public:
    struct PackEvent {
        uint64_t value = 0;
        int64_t tick = 0;
        int fif = 0;
    };

    SrtOutput(gpu::VkEngine& vk, media::CudaCtx& cuda, gpu::Compositor& comp,
              gpu::Timeline& renderTL, SrtOutConfig cfg,
              const VideoFormatDesc& show);
    ~SrtOutput();

    bool ok() const { return ok_; }
    bool push(const PackEvent& e) { return evts_.push(e); }
    uint64_t copiedValue(int fif) const { return copied_[fif].load(std::memory_order_acquire); }

    int64_t framesEncoded() const { return encoded_.load(std::memory_order_relaxed); }
    int64_t packetsSent() const { return sent_.load(std::memory_order_relaxed); }
    bool connected() const { return connected_.load(std::memory_order_relaxed); }

private:
    void encodeLoop(std::stop_token st);
    void muxLoop(std::stop_token st);
    bool openMux();
    void closeMux(bool writeTrailer);
    static int interruptCb(void* opaque);

    gpu::VkEngine& vk_;
    media::CudaCtx& cuda_;
    gpu::Compositor& comp_;
    gpu::Timeline& renderTL_;
    SrtOutConfig cfg_;
    VideoFormatDesc show_;

    media::CudaCtx::Imported imports_[gpu::Compositor::kFramesInFlight]{};
    media::FfmpegNvenc enc_;

    SpscRing<PackEvent> evts_{16};
    SpscRing<AVPacket*> pkts_{512};
    std::atomic<uint64_t> copied_[gpu::Compositor::kFramesInFlight]{};
    std::atomic<int64_t> encoded_{0}, sent_{0}, dropped_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};

    AVFormatContext* oc_ = nullptr;
    bool ok_ = false;

    std::jthread encodeThread_;
    std::jthread muxThread_;
};

}  // namespace moo
