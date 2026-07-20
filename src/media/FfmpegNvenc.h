/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
// SPS/PPS. MPEG-TS/SRT uses in-band headers; file recording requests
// GLOBAL_HEADER for Matroska codec configuration. PTS is the media tick index
// in show timebase (fpsD/fpsN).
class FfmpegNvenc {
public:
    ~FfmpegNvenc() { close(); }

    bool open(CudaCtx& cuda, const VideoFormatDesc& show, int bitrateKbps,
              bool globalHeader = false);
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
