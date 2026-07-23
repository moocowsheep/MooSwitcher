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
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/Spsc.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"
#include "media/AacEncoder.h"
#include "media/CudaCtx.h"
#include "media/FfmpegNvenc.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace moo {

// HEVC/AAC Matroska program recorder. It consumes the same per-FIF NV12
// buffers as SRT output, but owns an independent NVENC session and muxer.
// GPU copies, encoding, and file I/O stay off the render thread; a slow disk
// drops recorder packets/frames without disturbing program output.
class FileRecorder {
public:
    struct PackEvent {
        uint64_t value = 0;
        int64_t tick = 0;
        int fif = 0;
    };

    FileRecorder(media::CudaCtx& cuda, gpu::Compositor& comp,
                 gpu::Timeline& renderTL,
                 std::string path, const VideoFormatDesc& show,
                 bool withAudio, int64_t startTick, int bitrateKbps = 0,
                 gpu::Compositor::Feed feed =
                     gpu::Compositor::Feed::Program);
    ~FileRecorder();

    bool ok() const { return ok_; }
    bool push(const PackEvent& event) {
        return accepting_.load(std::memory_order_relaxed) && events_.push(event);
    }
    void pushAudio(const float* lr, int frames, int64_t firstSample);

    uint64_t copiedValue(int fif) const {
        return copied_[fif].load(std::memory_order_acquire);
    }
    int64_t framesEncoded() const {
        return encoded_.load(std::memory_order_relaxed);
    }
    int64_t packetsWritten() const {
        return written_.load(std::memory_order_relaxed);
    }
    bool failed() const { return failed_.load(std::memory_order_relaxed); }
    const std::string& path() const { return path_; }

private:
    bool openMux();
    void closeMux();
    void encodeLoop(std::stop_token stop);
    void muxLoop(std::stop_token stop);

    media::CudaCtx& cuda_;
    gpu::Compositor& comp_;
    gpu::Timeline& renderTL_;
    gpu::Compositor::Feed feed_;
    std::string statsPrefix_;
    std::string path_;
    VideoFormatDesc show_;
    int64_t startTick_ = 0;
    int64_t startSample_ = 0;

    media::CudaCtx::Imported imports_[gpu::Compositor::kFramesInFlight]{};
    media::FfmpegNvenc encoder_;
    media::AacEncoder aac_;
    std::vector<AVPacket*> aacScratch_;  // audio thread, then destructor only

    SpscRing<PackEvent> events_{32};
    SpscRing<AVPacket*> videoPackets_{1024};
    SpscRing<AVPacket*> audioPackets_{1024};
    std::atomic<uint64_t> copied_[gpu::Compositor::kFramesInFlight]{};
    std::atomic<int64_t> encoded_{0};
    std::atomic<int64_t> written_{0};
    std::atomic<int64_t> dropped_{0};
    std::atomic<bool> accepting_{false};
    std::atomic<bool> failed_{false};

    AVFormatContext* output_ = nullptr;
    int videoStream_ = -1;
    int audioStream_ = -1;
    bool headerWritten_ = false;
    bool ok_ = false;

    std::jthread encodeThread_;
    std::jthread muxThread_;
};

}  // namespace moo
