#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "engine/IInputSource.h"
#include "gpu/Nv12Ring.h"
#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace moo {

// SRT/HEVC (or H.264) ingest: libavformat reads the TS, NVDEC decodes into
// CUDA frames, one device-to-device copy lands NV12 planes in an Nv12Ring's
// exported staging buffer, and Vulkan copies to Y/UV images. Decode never
// touches the CPU pixel path. Reconnects with backoff; same latest-frame
// mailbox contract as the NDI receiver.
class SrtInput : public IInputSource {
public:
    SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, media::CudaCtx& cuda,
             std::string url, int index);
    ~SrtInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    Status status() const override;

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

private:
    void run(std::stop_token st);
    bool openStream();
    void closeStream();
    void handleFrame(AVFrame* f);
    static int interruptCb(void* opaque);
    static AVPixelFormat pickCuda(AVCodecContext*, const AVPixelFormat* fmts);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    media::CudaCtx& cuda_;
    std::string url_;
    int index_;

    AVBufferRef* hwDev_ = nullptr;
    AVFormatContext* ic_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    int vidIdx_ = -1;

    std::shared_ptr<gpu::Nv12Ring> ring_;
    media::CudaCtx::Imported imports_[gpu::Nv12Ring::kSlots]{};

    Mailbox mailbox_;
    std::atomic<audio::InputChannel*> audioSink_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int64_t> frames_{0}, drops_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
