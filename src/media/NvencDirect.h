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
 * runtime, and distribute the combined work. See EXCEPTIONS.md for the
 * full exception text. */
#pragma once
#include <cstdint>
#include <vector>

#include "media/IVideoEncoder.h"

extern "C" {
#include <ffnvcodec/nvEncodeAPI.h>
}

namespace moo::media {

// HEVC via the NVENC API in libnvidia-encode, bypassing libavcodec. Same tune
// as the FFmpeg backend (EncoderConfig::preset + ultra-low-latency, CBR,
// single-frame VBV, no B-frames, IDR ~2 s), but the render thread's NV12 pack buffers are
// REGISTERED with NVENC once and encoded in place -- no per-frame
// device-to-device copy into an encoder pool, which is a full frame of
// bandwidth saved per output (~50 MB at 8K).
//
// The session is synchronous: nvEncLockBitstream blocks until the picture is
// done, so `encode` returns with the input buffer already consumed, keeping
// the render thread's recycle handshake intact. Packets are copied into
// AVPackets so muxing stays identical across backends.
class NvencDirect final : public IVideoEncoder {
public:
    ~NvencDirect() override { close(); }

    bool open(CudaCtx& cuda, const VideoFormatDesc& show,
              const EncoderConfig& cfg) override;
    void close() override;
    bool ok() const override { return enc_ != nullptr; }

    AVRational timeBase() const override { return timeBase_; }
    bool fillCodecpar(AVCodecParameters* par) const override;

    bool encode(CUdeviceptr src, int64_t pts,
                std::vector<AVPacket*>& out) override;
    bool drain(std::vector<AVPacket*>& out) override;

private:
    // One registration per distinct pack buffer (kFramesInFlight of them).
    struct Registration {
        CUdeviceptr ptr = 0;
        NV_ENC_REGISTERED_PTR handle = nullptr;
    };

    NV_ENC_REGISTERED_PTR registrationFor(CUdeviceptr src);
    bool fetchExtradata();
    void logLast(const char* what, NVENCSTATUS status) const;

    CudaCtx* cuda_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api_{};
    void* enc_ = nullptr;
    NV_ENC_CONFIG cfg_{};
    NV_ENC_INITIALIZE_PARAMS init_{};

    std::vector<Registration> regs_;
    std::vector<NV_ENC_OUTPUT_PTR> bitstreams_;
    size_t nextBitstream_ = 0;
    std::vector<uint8_t> extradata_;

    AVRational timeBase_{1, 1};
    int w_ = 0, h_ = 0;
    int64_t bitrate_ = 0;
    uint32_t frameIdx_ = 0;
    bool eosSent_ = false;
};

}  // namespace moo::media
