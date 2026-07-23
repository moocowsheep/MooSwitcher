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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/IInputSource.h"
#include "gpu/Nv12Ring.h"
#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"
#include "media/Playlist.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace moo {

// FFmpeg/NVDEC ingest for live SRT or paced local media files. libavformat
// reads the source, NVDEC decodes into CUDA frames, and one device-to-device
// copy lands NV12 planes in an Nv12Ring's
// exported staging buffer, and Vulkan copies to Y/UV images. Decode never
// touches the CPU pixel path. The source's audio stream (if any) is decoded on
// the CPU and swresampled to the mixer's 48 kHz stereo f32. Reconnects with
// backoff for SRT; local files are paced from their media timestamps and
// expose play/pause/restart/loop transport through IInputSource.
class SrtInput : public IInputSource {
public:
    // syncFrames >= 0 enables the frame-sync feed (and sizes the NV12 ring
    // for that many queued frames); -1 = plain latest-frame input.
    SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, media::CudaCtx& cuda,
             std::string url, int index, int syncFrames = -1,
             bool mediaMode = false, bool mediaPlaying = true,
             bool mediaLoop = true,
             std::vector<media::PlaylistItem> mediaPlaylist = {});
    ~SrtInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }
    Status status() const override;
    MediaState mediaState() const override;
    void setMediaPlaying(bool playing) override;
    void setMediaLoop(bool loop) override;
    void restartMedia() override;
    void stepMedia(int direction) override;

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
    bool initAudioTempo(const AVFrame* f, int speedPermille);
    void closeAudioTempo();
    void drainAudioTempo();
    bool paceTimestamp(int streamIndex, int64_t timestamp,
                       std::stop_token stop);
    int64_t mediaTimestampMs(int streamIndex, int64_t timestamp) const;
    bool advanceMedia();
    void selectMedia(int index);
    media::PlaylistItem currentMediaItem() const;
    std::string currentMediaRef() const;
    static int interruptCb(void* opaque);
    static AVPixelFormat pickCuda(AVCodecContext*, const AVPixelFormat* fmts);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    media::CudaCtx& cuda_;
    std::string url_;
    int index_;
    const bool mediaMode_;

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
    AVFilterGraph* afGraph_ = nullptr;
    AVFilterContext* afSource_ = nullptr;
    AVFilterContext* afSink_ = nullptr;
    AVFrame* afFrame_ = nullptr;
    int afSpeedPermille_ = 1000;

    std::shared_ptr<gpu::Nv12Ring> ring_;
    std::vector<media::CudaCtx::Imported> imports_;  // one per ring slot

    Mailbox mailbox_;
    const int syncFrames_;
    SyncFeed feed_{16};
    uint64_t pubSeq_ = 0;    // decode thread
    bool ptsSynth_ = false;  // decode thread; sticky per stream
    int64_t synthBaseNs_ = 0, synthK_ = 0, synthLastArrNs_ = 0;
    std::atomic<bool> mediaPlaying_{true};
    std::atomic<bool> mediaLoop_{true};
    std::atomic<bool> mediaRestart_{false};
    std::vector<media::PlaylistItem> mediaPlaylist_;
    std::atomic<int> mediaIndex_{0};
    std::atomic<int> mediaSelect_{-1};
    std::atomic<bool> mediaAtEnd_{false};
    std::atomic<int64_t> mediaPositionMs_{0};
    std::atomic<int64_t> mediaDurationMs_{0};
    int64_t mediaFirstPtsNs_ = INT64_MIN;  // decode thread
    int64_t mediaStartMonoNs_ = 0;
    int64_t mediaPauseMonoNs_ = 0;
    bool mediaWasPlaying_ = false;
    bool mediaClipDone_ = false;
    std::atomic<audio::InputChannel*> audioSink_{nullptr};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int64_t> frames_{0}, drops_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

class MediaInput final : public SrtInput {
public:
    MediaInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
               media::CudaCtx& cuda, std::string path, int index,
               int syncFrames = -1, bool playing = true, bool loop = true,
               std::vector<media::PlaylistItem> playlist = {})
        : SrtInput(eng, uploadQueue, cuda, std::move(path), index, syncFrames,
                   true, playing, loop, std::move(playlist)) {}
};

}  // namespace moo
