#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/IInputSource.h"
#include "gpu/Nv12Ring.h"
#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace moo {

// SRT/HEVC (or H.264) ingest: libavformat reads the TS, NVDEC decodes into
// CUDA frames, one device-to-device copy lands NV12 planes in an Nv12Ring's
// exported staging buffer, and Vulkan copies to Y/UV images. Decode never
// touches the CPU pixel path. The TS's audio stream (if any) is decoded on
// the CPU and swresampled to the mixer's 48 kHz stereo f32. Reconnects with
// backoff; same latest-frame mailbox contract as the NDI receiver.
class SrtInput : public IInputSource {
public:
    // syncFrames >= 0 enables the frame-sync feed (and sizes the NV12 ring
    // for that many queued frames); -1 = plain latest-frame input.
    SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, media::CudaCtx& cuda,
             std::string url, int index, int syncFrames = -1);
    ~SrtInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }
    Status status() const override;

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

    SyncFeed* syncFeed() override { return syncFrames_ >= 0 ? &feed_ : nullptr; }

private:
    void run(std::stop_token st);
    bool openStream();
    void closeStream();
    void handleFrame(AVFrame* f);
    void handleAudioFrame(AVFrame* f);
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

    AVCodecContext* adec_ = nullptr;
    int audIdx_ = -1;
    SwrContext* swr_ = nullptr;
    AVChannelLayout swrInLayout_{};
    int swrInRate_ = 0;
    int swrInFmt_ = -1;
    std::vector<float> aconv_;  // decode-thread scratch

    std::shared_ptr<gpu::Nv12Ring> ring_;
    std::vector<media::CudaCtx::Imported> imports_;  // one per ring slot

    Mailbox mailbox_;
    const int syncFrames_;
    SyncFeed feed_{16};
    uint64_t pubSeq_ = 0;    // decode thread
    bool ptsSynth_ = false;  // decode thread; sticky per stream
    int64_t synthBaseNs_ = 0, synthK_ = 0, synthLastArrNs_ = 0;
    std::atomic<audio::InputChannel*> audioSink_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int64_t> frames_{0}, drops_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
