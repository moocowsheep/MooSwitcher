/* MooSwitcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * Additional permission under GNU GPL version 3 section 7: you may link
 * MooSwitcher against the proprietary NDI SDK, the NVIDIA CUDA / Video
 * Codec SDK runtime (CUDA, NVENC, NVDEC), and the OMT (libomt / libvmx)
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

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
