#pragma once
#include <cstdint>
#include <vector>

#include "core/Format.h"
#include "media/CudaCtx.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace moo::media {

// hevc_nvenc via CUDA hwframes built on OUR primary context. Low-latency
// tuned: p4 + tune ull, CBR with single-frame VBV, no B-frames, in-band
// SPS/PPS (no GLOBAL_HEADER, so nvenc repeats headers on every IDR — what
// MPEG-TS wants). PTS is the media tick index in show timebase (fpsD/fpsN).
class FfmpegNvenc {
public:
    ~FfmpegNvenc() { close(); }

    bool open(CudaCtx& cuda, const VideoFormatDesc& show, int bitrateKbps);
    void close();
    bool ok() const { return enc_ != nullptr; }

    const AVCodecContext* codecCtx() const { return enc_; }
    AVRational timeBase() const { return enc_ ? enc_->time_base : AVRational{1, 1}; }

    // Copies tight-pitch NV12 planes from `src` (device ptr) into a pool
    // frame (synchronized before return -> src is reusable) and encodes.
    // Emitted packets are appended to `out` (caller frees).
    bool encode(CUdeviceptr src, int64_t pts, std::vector<AVPacket*>& out);
    bool drain(std::vector<AVPacket*>& out);

private:
    bool receiveAll(std::vector<AVPacket*>& out);

    CudaCtx* cuda_ = nullptr;
    AVBufferRef* hwDev_ = nullptr;
    AVBufferRef* hwFrames_ = nullptr;
    AVCodecContext* enc_ = nullptr;
    AVFrame* frame_ = nullptr;
    int w_ = 0, h_ = 0;
};

}  // namespace moo::media
