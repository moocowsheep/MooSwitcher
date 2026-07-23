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

#include "out/FileRecorder.h"

#include <mutex>

#include "audio/MixerCore.h"
#include "core/Log.h"
#include "core/Stats.h"

namespace moo {

FileRecorder::FileRecorder(media::CudaCtx& cuda, gpu::Compositor& comp,
                           gpu::Timeline& renderTL,
                           std::string path, const VideoFormatDesc& show,
                           bool withAudio, int64_t startTick, int bitrateKbps,
                           gpu::Compositor::Feed feed)
    : cuda_(cuda), comp_(comp), renderTL_(renderTL), feed_(feed),
      statsPrefix_(feed == gpu::Compositor::Feed::Clean ? "cleanRecord"
                                                       : "record"),
      path_(std::move(path)), show_(show), startTick_(startTick) {
    static std::once_flag networkInit;
    std::call_once(networkInit, [] { avformat_network_init(); });

    if (!encoder_.open(cuda_, show_, bitrateKbps, true)) return;
    startSample_ = av_rescale_q(startTick_, encoder_.timeBase(),
                                AVRational{1, audio::kSampleRate});
    if (withAudio && !aac_.open(audio::kSampleRate, 160'000))
        MOO_LOGW("record: AAC encoder unavailable; recording video-only");

    for (int fif = 0; fif < gpu::Compositor::kFramesInFlight; ++fif) {
        const int fd = comp_.nvPackExportFd(fif, feed_);
        if (fd < 0 ||
            !cuda_.importVkFd(fd, comp_.nvPackBytes(), imports_[fif])) {
            MOO_LOGE("record: NV12 pack import failed (fif %d)", fif);
            return;
        }
    }
    if (!openMux()) return;

    accepting_.store(true, std::memory_order_relaxed);
    ok_ = true;
    encodeThread_ =
        std::jthread([this](std::stop_token stop) { encodeLoop(stop); });
    muxThread_ = std::jthread([this](std::stop_token stop) { muxLoop(stop); });
    MOO_LOGI("%s: started '%s'", statsPrefix_.c_str(), path_.c_str());
}

FileRecorder::~FileRecorder() {
    accepting_.store(false, std::memory_order_relaxed);

    if (encodeThread_.joinable()) {
        encodeThread_.request_stop();
        encodeThread_.join();
    }

    // No audio callback can still own this object: callers take shared
    // ownership before entering pushAudio. Finish the last complete AAC
    // frames before asking the mux thread to drain its packet rings.
    if (aac_.ok()) {
        aacScratch_.clear();
        aac_.drain(aacScratch_);
        for (auto* packet : aacScratch_)
            if (!audioPackets_.push(packet)) av_packet_free(&packet);
    }

    if (muxThread_.joinable()) {
        muxThread_.request_stop();
        muxThread_.join();
    } else {
        closeMux();
    }

    AVPacket* packet = nullptr;
    while (videoPackets_.pop(packet)) av_packet_free(&packet);
    while (audioPackets_.pop(packet)) av_packet_free(&packet);
    for (auto& imported : imports_) cuda_.release(imported);
}

bool FileRecorder::openMux() {
    if (avformat_alloc_output_context2(&output_, nullptr, "matroska",
                                       path_.c_str()) < 0 ||
        !output_) {
        MOO_LOGE("record: cannot create Matroska muxer for '%s'", path_.c_str());
        return false;
    }

    AVStream* video = avformat_new_stream(output_, nullptr);
    if (!video) return false;
    if (avcodec_parameters_from_context(video->codecpar,
                                        encoder_.codecCtx()) < 0)
        return false;
    video->time_base = encoder_.timeBase();
    videoStream_ = video->index;

    if (aac_.ok()) {
        AVStream* audioStream = avformat_new_stream(output_, nullptr);
        if (!audioStream) return false;
        if (avcodec_parameters_from_context(audioStream->codecpar,
                                            aac_.codecCtx()) < 0)
            return false;
        audioStream->time_base = aac_.timeBase();
        audioStream_ = audioStream->index;
        output_->max_interleave_delta = 500'000;
    }

    if (!(output_->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&output_->pb, path_.c_str(), AVIO_FLAG_WRITE) < 0) {
        MOO_LOGE("record: cannot open '%s' for writing", path_.c_str());
        return false;
    }
    const int headerResult = avformat_write_header(output_, nullptr);
    if (headerResult < 0) {
        char error[128];
        av_strerror(headerResult, error, sizeof error);
        MOO_LOGE("record: Matroska header failed for '%s': %s", path_.c_str(),
                 error);
        return false;
    }
    headerWritten_ = true;
    return true;
}

void FileRecorder::closeMux() {
    if (!output_) return;
    if (output_->pb) {
        if (headerWritten_ && av_write_trailer(output_) < 0)
            MOO_LOGW("record: trailer failed for '%s'", path_.c_str());
        avio_closep(&output_->pb);
    }
    avformat_free_context(output_);
    output_ = nullptr;
    headerWritten_ = false;
}

void FileRecorder::pushAudio(const float* lr, int frames,
                             int64_t firstSample) {
    if (!accepting_.load(std::memory_order_relaxed) || !aac_.ok()) return;
    constexpr int64_t kAudioPtsBiasSamples = 400;
    aacScratch_.clear();
    if (!aac_.encode(lr, frames,
                     firstSample + kAudioPtsBiasSamples - startSample_,
                     aacScratch_)) {
        for (auto* packet : aacScratch_) av_packet_free(&packet);
        return;
    }
    for (auto* packet : aacScratch_) {
        if (!audioPackets_.push(packet)) {
            av_packet_free(&packet);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            Stats::counter(statsPrefix_ + ".audioPacketDrops").add();
        }
    }
}

void FileRecorder::encodeLoop(std::stop_token stop) {
    auto& encodedCounter = Stats::counter(statsPrefix_ + ".frames");
    auto& waitCounter =
        Stats::counter(statsPrefix_ + ".renderWaitTimeouts");
    auto& dropCounter = Stats::counter(statsPrefix_ + ".videoPacketDrops");
    cuda_.makeCurrent();
    std::vector<AVPacket*> packets;

    while (!stop.stop_requested() || events_.size()) {
        PackEvent event;
        if (!events_.pop(event)) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        while (!renderTL_.waitCompleted(event.value, 100'000'000)) {
            waitCounter.add();
        }

        packets.clear();
        const int64_t pts = event.tick + 1 - startTick_;
        if (encoder_.encode(imports_[event.fif].ptr, pts, packets)) {
            copied_[event.fif].store(event.value, std::memory_order_release);
            encoded_.fetch_add(1, std::memory_order_relaxed);
            encodedCounter.add();
            for (auto* packet : packets) {
                if (!videoPackets_.push(packet)) {
                    av_packet_free(&packet);
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                    dropCounter.add();
                }
            }
        } else {
            copied_[event.fif].store(event.value, std::memory_order_release);
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    packets.clear();
    encoder_.drain(packets);
    for (auto* packet : packets)
        if (!videoPackets_.push(packet)) av_packet_free(&packet);
}

void FileRecorder::muxLoop(std::stop_token stop) {
    auto& packetCounter = Stats::counter(statsPrefix_ + ".packets");
    const AVRational videoTimeBase = encoder_.timeBase();
    const AVRational audioTimeBase = aac_.timeBase();

    while (!stop.stop_requested() || videoPackets_.size() ||
           audioPackets_.size()) {
        AVPacket* packet = nullptr;
        int streamIndex = videoStream_;
        AVRational packetTimeBase = videoTimeBase;
        if (!videoPackets_.pop(packet) && aac_.ok() &&
            audioPackets_.pop(packet)) {
            streamIndex = audioStream_;
            packetTimeBase = audioTimeBase;
        }
        if (!packet) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        packet->stream_index = streamIndex;
        av_packet_rescale_ts(
            packet, packetTimeBase,
            output_->streams[size_t(streamIndex)]->time_base);
        const int result = av_interleaved_write_frame(output_, packet);
        av_packet_free(&packet);
        if (result < 0) {
            failed_.store(true, std::memory_order_relaxed);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            MOO_LOGE("record: write failed (%d) for '%s'", result,
                     path_.c_str());
        } else {
            written_.fetch_add(1, std::memory_order_relaxed);
            packetCounter.add();
        }
    }
    closeMux();
    MOO_LOGI("%s: stopped '%s' (%lld frames, %lld packets%s)",
             statsPrefix_.c_str(), path_.c_str(), (long long)framesEncoded(),
             (long long)packetsWritten(), failed() ? ", errors" : "");
}

}  // namespace moo
