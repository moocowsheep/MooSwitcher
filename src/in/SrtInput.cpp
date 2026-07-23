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

#include "in/SrtInput.h"

#include <cmath>
#include <cstdio>
#include <mutex>

#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"
#include "media/Playlist.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext_cuda.h>
}

namespace moo {

SrtInput::SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                   media::CudaCtx& cuda, std::string url, int index,
                   int syncFrames, bool mediaMode, bool mediaPlaying,
                   bool mediaLoop,
                   std::vector<media::PlaylistItem> mediaPlaylist)
    : eng_(eng), queue_(uploadQueue), cuda_(cuda), url_(std::move(url)),
      index_(index), mediaMode_(mediaMode), syncFrames_(syncFrames),
      mediaPlaying_(mediaPlaying), mediaLoop_(mediaLoop),
      mediaPlaylist_(std::move(mediaPlaylist)) {
    if (mediaMode_ && mediaPlaylist_.empty() && !url_.empty())
        mediaPlaylist_.emplace_back(url_);
    for (auto& item : mediaPlaylist_)
        media::normalizePlaylistItem(item);
    static std::once_flag netInit;
    std::call_once(netInit, [] { avformat_network_init(); });

    hwDev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (hwDev_) {
        auto* dctx = reinterpret_cast<AVHWDeviceContext*>(hwDev_->data);
        static_cast<AVCUDADeviceContext*>(dctx->hwctx)->cuda_ctx = cuda_.ctx();
        if (av_hwdevice_ctx_init(hwDev_) < 0) av_buffer_unref(&hwDev_);
    }
    if (!hwDev_) {
        MOO_LOGE("in%d(%s): CUDA hwdevice init failed", index_,
                 mediaMode_ ? "media" : "srt");
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

SrtInput::~SrtInput() {
    stopFlag_.store(true);  // aborts blocking avformat calls
    thread_ = {};
    closeStream();
    for (auto& im : imports_) cuda_.release(im);
    if (hwDev_) av_buffer_unref(&hwDev_);
}

SrtInput::Status SrtInput::status() const {
    Status s;
    s.connected = connected_.load(std::memory_order_relaxed);
    s.frames = frames_.load(std::memory_order_relaxed);
    s.drops = drops_.load(std::memory_order_relaxed);
    std::lock_guard lk(descM_);
    s.desc = desc_;
    return s;
}

IInputSource::MediaState SrtInput::mediaState() const {
    if (!mediaMode_) return {};
    const int playlistSize = int(mediaPlaylist_.size());
    const int playlistIndex = std::clamp(
        mediaIndex_.load(std::memory_order_relaxed), 0,
        std::max(0, playlistSize - 1));
    const auto item = playlistSize > 0
                          ? mediaPlaylist_[size_t(playlistIndex)]
                          : media::PlaylistItem{};
    return {true,
            mediaPlaying_.load(std::memory_order_relaxed),
            mediaLoop_.load(std::memory_order_relaxed),
            mediaAtEnd_.load(std::memory_order_relaxed),
            mediaPositionMs_.load(std::memory_order_relaxed),
            mediaDurationMs_.load(std::memory_order_relaxed),
            playlistIndex,
            playlistSize,
            item.inMs,
            item.outMs,
            item.speedPermille,
            item.path};
}

void SrtInput::setMediaPlaying(bool playing) {
    if (!mediaMode_) return;
    if (playing && mediaAtEnd_.load(std::memory_order_relaxed)) {
        selectMedia(0);
        mediaRestart_.store(true, std::memory_order_release);
    }
    mediaPlaying_.store(playing, std::memory_order_release);
}

void SrtInput::setMediaLoop(bool loop) {
    if (mediaMode_) mediaLoop_.store(loop, std::memory_order_release);
}

void SrtInput::restartMedia() {
    if (!mediaMode_) return;
    mediaPlaying_.store(true, std::memory_order_release);
    mediaRestart_.store(true, std::memory_order_release);
}

void SrtInput::selectMedia(int index) {
    if (!mediaMode_ || mediaPlaylist_.empty()) return;
    mediaSelect_.store(
        std::clamp(index, 0, int(mediaPlaylist_.size()) - 1),
        std::memory_order_release);
}

void SrtInput::stepMedia(int direction) {
    if (!mediaMode_ || direction == 0 || mediaPlaylist_.empty()) return;
    const int pending = mediaSelect_.load(std::memory_order_acquire);
    const int current = pending >= 0
                            ? pending
                            : mediaIndex_.load(std::memory_order_relaxed);
    selectMedia(int(media::playlistStep(size_t(std::max(0, current)),
                                        mediaPlaylist_.size(), direction)));
    mediaPlaying_.store(true, std::memory_order_release);
}

std::string SrtInput::currentMediaRef() const {
    return currentMediaItem().path;
}

media::PlaylistItem SrtInput::currentMediaItem() const {
    if (!mediaMode_ || mediaPlaylist_.empty())
        return media::PlaylistItem{url_};
    const int index = std::clamp(
        mediaIndex_.load(std::memory_order_relaxed), 0,
        int(mediaPlaylist_.size()) - 1);
    return mediaPlaylist_[size_t(index)];
}

int SrtInput::interruptCb(void* opaque) {
    return static_cast<SrtInput*>(opaque)->stopFlag_.load(std::memory_order_relaxed);
}

AVPixelFormat SrtInput::pickCuda(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_CUDA) return *p;
    return fmts[0];
}

bool SrtInput::openStream() {
    const auto mediaItem = currentMediaItem();
    ptsSynth_ = false;  // re-judge the new stream's timestamps
    synthK_ = 0;
    mediaFirstPtsNs_ = INT64_MIN;
    mediaStartMonoNs_ = 0;
    mediaPauseMonoNs_ = 0;
    mediaWasPlaying_ = false;
    mediaClipDone_ = false;
    mediaAtEnd_.store(false, std::memory_order_relaxed);
    mediaPositionMs_.store(mediaMode_ ? mediaItem.inMs : 0,
                           std::memory_order_relaxed);
    ic_ = avformat_alloc_context();
    ic_->interrupt_callback = {&SrtInput::interruptCb, this};
    const std::string source = currentMediaRef();
    if (avformat_open_input(&ic_, source.c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(ic_, nullptr) < 0) return false;
    vidIdx_ = av_find_best_stream(ic_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidIdx_ < 0) return false;

    const AVCodecParameters* par = ic_->streams[vidIdx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    dec_ = avcodec_alloc_context3(codec);
    if (avcodec_parameters_to_context(dec_, par) < 0) return false;
    dec_->hw_device_ctx = av_buffer_ref(hwDev_);
    dec_->get_format = &SrtInput::pickCuda;
    if (avcodec_open2(dec_, codec, nullptr) < 0) return false;

    // Audio is optional: decode on the CPU when the TS carries a stream.
    audIdx_ = av_find_best_stream(ic_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audIdx_ >= 0) {
        const AVCodecParameters* apar = ic_->streams[audIdx_]->codecpar;
        if (const AVCodec* acodec = avcodec_find_decoder(apar->codec_id)) {
            adec_ = avcodec_alloc_context3(acodec);
            if (avcodec_parameters_to_context(adec_, apar) < 0 ||
                avcodec_open2(adec_, acodec, nullptr) < 0)
                avcodec_free_context(&adec_);
        }
        if (!adec_) {
            MOO_LOGW("in%d(%s): audio stream present but not decodable",
                     index_, mediaMode_ ? "media" : "srt");
            audIdx_ = -1;
        }
    }

    if (mediaMode_) {
        const int64_t duration =
            ic_->duration > 0 && ic_->duration != AV_NOPTS_VALUE
                ? ic_->duration / (AV_TIME_BASE / 1000)
                : 0;
        mediaDurationMs_.store(duration, std::memory_order_relaxed);

        if (mediaItem.inMs > 0) {
            const int64_t originUs =
                ic_->start_time != AV_NOPTS_VALUE ? ic_->start_time : 0;
            const int64_t targetUs = originUs + mediaItem.inMs * 1000;
            if (avformat_seek_file(ic_, -1, INT64_MIN, targetUs, INT64_MAX,
                                   AVSEEK_FLAG_BACKWARD) < 0) {
                MOO_LOGW("in%d(media): seek to trim-in %lld ms failed; "
                         "decoding forward",
                         index_, (long long)mediaItem.inMs);
            }
            avcodec_flush_buffers(dec_);
            if (adec_) avcodec_flush_buffers(adec_);
        }
    }
    connected_.store(true, std::memory_order_relaxed);
    if (mediaMode_) {
        const std::string outLabel =
            mediaItem.outMs > 0 ? std::to_string(mediaItem.outMs) : "end";
        MOO_LOGI("in%d(media): opened item %d/%zu '%s', trim %lld..%s, "
                 "speed %.2fx, %s %dx%d%s",
                 index_,
                 mediaIndex_.load(std::memory_order_relaxed) + 1,
                 mediaPlaylist_.size(), mediaItem.path.c_str(),
                 (long long)mediaItem.inMs, outLabel.c_str(),
                 double(mediaItem.speedPermille) / 1000.0,
                 codec->name, par->width, par->height,
                 adec_ ? " + audio" : "");
    } else {
        MOO_LOGI("in%d(srt): opened, %s %dx%d%s", index_, codec->name,
                 par->width, par->height, adec_ ? " + audio" : "");
    }
    return true;
}

int64_t SrtInput::mediaTimestampMs(int streamIndex, int64_t timestamp) const {
    if (!mediaMode_ || !ic_ || streamIndex < 0 ||
        streamIndex >= int(ic_->nb_streams) || timestamp == AV_NOPTS_VALUE)
        return -1;
    const AVStream* stream = ic_->streams[streamIndex];
    const int64_t ptsUs =
        av_rescale_q(timestamp, stream->time_base, AVRational{1, AV_TIME_BASE});
    int64_t originUs = 0;
    if (ic_->start_time != AV_NOPTS_VALUE) {
        originUs = ic_->start_time;
    } else if (stream->start_time != AV_NOPTS_VALUE) {
        originUs = av_rescale_q(stream->start_time, stream->time_base,
                                AVRational{1, AV_TIME_BASE});
    }
    return std::max<int64_t>(0, (ptsUs - originUs) / 1000);
}

bool SrtInput::advanceMedia() {
    if (!mediaMode_) return false;
    if (afSource_) {
        const int flushResult =
            av_buffersrc_add_frame_flags(afSource_, nullptr, 0);
        (void)flushResult;
        drainAudioTempo();
        closeAudioTempo();
    }
    const size_t current = size_t(
        std::max(0, mediaIndex_.load(std::memory_order_relaxed)));
    const auto next = media::playlistAdvance(
        current, mediaPlaylist_.size(),
        mediaLoop_.load(std::memory_order_acquire));
    mediaClipDone_ = false;
    if (next) {
        mediaIndex_.store(int(*next), std::memory_order_relaxed);
        closeStream();
        return true;
    }
    mediaAtEnd_.store(true, std::memory_order_relaxed);
    mediaPlaying_.store(false, std::memory_order_release);
    return false;
}

void SrtInput::closeStream() {
    closeAudioTempo();
    if (dec_) avcodec_free_context(&dec_);
    if (adec_) avcodec_free_context(&adec_);
    if (swr_) swr_free(&swr_);
    av_channel_layout_uninit(&swrInLayout_);
    swrInRate_ = 0;
    swrInFmt_ = -1;
    if (ic_) avformat_close_input(&ic_);
    vidIdx_ = -1;
    audIdx_ = -1;
    connected_.store(false, std::memory_order_relaxed);
}

bool SrtInput::initAudioTempo(const AVFrame* f, int speedPermille) {
    if (afGraph_ && afSpeedPermille_ == speedPermille) return true;
    closeAudioTempo();
    if (!f || f->sample_rate <= 0 || f->ch_layout.nb_channels <= 0)
        return false;

    char layout[128]{};
    if (av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout)) < 0)
        return false;
    const char* sampleFmt = av_get_sample_fmt_name(AVSampleFormat(f->format));
    if (!sampleFmt) return false;

    afGraph_ = avfilter_graph_alloc();
    if (!afGraph_) return false;
    char sourceArgs[512];
    const AVRational timeBase = ic_->streams[audIdx_]->time_base;
    snprintf(sourceArgs, sizeof(sourceArgs),
             "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:"
             "channel_layout=%s",
             timeBase.num, timeBase.den, f->sample_rate, sampleFmt, layout);

    int err = avfilter_graph_create_filter(
        &afSource_, avfilter_get_by_name("abuffer"), "media_audio_in",
        sourceArgs, nullptr, afGraph_);
    if (err >= 0)
        err = avfilter_graph_create_filter(
            &afSink_, avfilter_get_by_name("abuffersink"),
            "media_audio_out", nullptr, nullptr, afGraph_);
    if (err < 0) {
        closeAudioTempo();
        return false;
    }

    std::string chain;
    double remaining = double(speedPermille) / 1000.0;
    const auto appendTempo = [&](double tempo) {
        char node[48];
        snprintf(node, sizeof(node), "%satempo=%.6f",
                 chain.empty() ? "" : ",", tempo);
        chain += node;
    };
    while (remaining < 0.5 - 1e-9) {
        appendTempo(0.5);
        remaining /= 0.5;
    }
    while (remaining > 2.0 + 1e-9) {
        appendTempo(2.0);
        remaining /= 2.0;
    }
    if (std::abs(remaining - 1.0) > 1e-9 || chain.empty())
        appendTempo(remaining);
    chain +=
        ",aformat=sample_fmts=flt:sample_rates=48000:"
        "channel_layouts=stereo";

    AVFilterInOut* graphInputs = avfilter_inout_alloc();
    AVFilterInOut* graphOutputs = avfilter_inout_alloc();
    if (!graphInputs || !graphOutputs) {
        avfilter_inout_free(&graphInputs);
        avfilter_inout_free(&graphOutputs);
        closeAudioTempo();
        return false;
    }
    graphOutputs->name = av_strdup("in");
    graphOutputs->filter_ctx = afSource_;
    graphOutputs->pad_idx = 0;
    graphOutputs->next = nullptr;
    graphInputs->name = av_strdup("out");
    graphInputs->filter_ctx = afSink_;
    graphInputs->pad_idx = 0;
    graphInputs->next = nullptr;

    err = avfilter_graph_parse_ptr(afGraph_, chain.c_str(), &graphInputs,
                                   &graphOutputs, nullptr);
    avfilter_inout_free(&graphInputs);
    avfilter_inout_free(&graphOutputs);
    if (err >= 0) err = avfilter_graph_config(afGraph_, nullptr);
    if (err < 0) {
        MOO_LOGW("in%d(media): audio tempo graph init failed (%d)",
                 index_, err);
        closeAudioTempo();
        return false;
    }
    afFrame_ = av_frame_alloc();
    afSpeedPermille_ = speedPermille;
    return afFrame_ != nullptr;
}

void SrtInput::drainAudioTempo() {
    if (!afSink_ || !afFrame_) return;
    auto* ch = audioSink_.load(std::memory_order_acquire);
    while (av_buffersink_get_frame(afSink_, afFrame_) >= 0) {
        if (ch && afFrame_->format == AV_SAMPLE_FMT_FLT &&
            afFrame_->ch_layout.nb_channels == audio::kChannels &&
            afFrame_->nb_samples > 0) {
            const int64_t pts =
                afFrame_->pts != AV_NOPTS_VALUE
                    ? av_rescale_q(afFrame_->pts,
                                   av_buffersink_get_time_base(afSink_),
                                   AVRational{1, 1'000'000'000})
                    : audio::InputChannel::kNoPts;
            ch->pushInterleaved(
                reinterpret_cast<const float*>(afFrame_->data[0]),
                afFrame_->nb_samples, audio::kSampleRate,
                pts);
        }
        av_frame_unref(afFrame_);
    }
}

void SrtInput::closeAudioTempo() {
    if (afFrame_) av_frame_free(&afFrame_);
    if (afGraph_) avfilter_graph_free(&afGraph_);
    afSource_ = nullptr;
    afSink_ = nullptr;
    afSpeedPermille_ = 1000;
}

void SrtInput::handleAudioFrame(AVFrame* f) {
    auto* ch = audioSink_.load(std::memory_order_acquire);
    if (!ch || f->nb_samples <= 0) return;

    const int speedPermille =
        mediaMode_ ? currentMediaItem().speedPermille : 1000;
    if (mediaMode_ && speedPermille != 1000) {
        if (!initAudioTempo(f, speedPermille)) return;
        AVFrame* input = av_frame_clone(f);
        if (!input) return;
        input->pts = f->best_effort_timestamp;
        const int err = av_buffersrc_add_frame_flags(
            afSource_, input, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_free(&input);
        if (err >= 0) {
            drainAudioTempo();
        } else {
            MOO_LOGW("in%d(media): audio tempo input failed (%d)",
                     index_, err);
        }
        return;
    }

    if (!swr_ || f->sample_rate != swrInRate_ || f->format != swrInFmt_ ||
        av_channel_layout_compare(&f->ch_layout, &swrInLayout_) != 0) {
        if (swr_) swr_free(&swr_);
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLT,
                                audio::kSampleRate, &f->ch_layout,
                                AVSampleFormat(f->format), f->sample_rate, 0,
                                nullptr) < 0 ||
            swr_init(swr_) < 0) {
            if (swr_) swr_free(&swr_);
            MOO_LOGW("in%d(%s): swr init failed for audio", index_,
                     mediaMode_ ? "media" : "srt");
            return;
        }
        av_channel_layout_copy(&swrInLayout_, &f->ch_layout);
        swrInRate_ = f->sample_rate;
        swrInFmt_ = f->format;
    }

    const int outCap = int(av_rescale_rnd(
        swr_get_delay(swr_, f->sample_rate) + f->nb_samples,
        audio::kSampleRate, f->sample_rate, AV_ROUND_UP));
    aconv_.resize(size_t(outCap) * audio::kChannels);
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(aconv_.data());
    const int got =
        swr_convert(swr_, &outPtr, outCap,
                    const_cast<const uint8_t**>(f->extended_data),
                    f->nb_samples);
    if (got > 0) {
        // Same TS clock as the video pts; swr's sub-chunk latency is a
        // constant that folds into the trim bias.
        const int64_t bet = f->best_effort_timestamp;
        const int64_t pts =
            bet != AV_NOPTS_VALUE
                ? av_rescale_q(bet, ic_->streams[audIdx_]->time_base,
                               AVRational{1, 1'000'000'000})
                : audio::InputChannel::kNoPts;
        ch->pushInterleaved(aconv_.data(), got, audio::kSampleRate, pts);
    }
}

void SrtInput::handleFrame(AVFrame* f) {
    auto& dropCtr = Stats::counter("in" + std::to_string(index_) + ".drops");
    auto& frameCtr = Stats::counter("in" + std::to_string(index_) + ".frames");

    if (f->format != AV_PIX_FMT_CUDA) return;

    VideoFormatDesc d;
    d.width = f->width;
    d.height = f->height;
    d.pixfmt = PixFmt::NV12;
    const AVRational fr =
        av_guess_frame_rate(ic_, ic_->streams[vidIdx_], f);
    if (fr.num > 0 && fr.den > 0) {
        d.fpsN = fr.num;
        d.fpsD = fr.den;
    }
    d.colorimetry = VideoFormatDesc::colorimetryForHeight(f->height);

    if (!ring_ || !(ring_->desc() == d)) {
        for (auto& im : imports_) cuda_.release(im);
        const int slots = gpu::Nv12Ring::kSlots +
                          (syncFrames_ >= 0 ? syncFrames_ + 2 : 0);
        ring_ = std::make_shared<gpu::Nv12Ring>(eng_, d, queue_, slots);
        imports_.assign(size_t(slots), media::CudaCtx::Imported{});
        for (int s = 0; s < slots; ++s) {
            const int fd = ring_->exportStagingFd(s);
            if (fd < 0 || !cuda_.importVkFd(fd, ring_->stagingBytes(), imports_[s])) {
                MOO_LOGE("in%d(%s): staging import failed", index_,
                         mediaMode_ ? "media" : "srt");
                ring_.reset();
                return;
            }
        }
        std::lock_guard lk(descM_);
        desc_ = d;
        MOO_LOGI("in%d(%s): format %dx%d @ %lld/%lld", index_,
                 mediaMode_ ? "media" : "srt", d.width, d.height,
                 (long long)d.fpsN, (long long)d.fpsD);
    }

    const int slot = ring_->acquire();
    if (slot < 0) {
        drops_.fetch_add(1, std::memory_order_relaxed);
        dropCtr.add();
        return;
    }

    cuda_.makeCurrent();
    CUDA_MEMCPY2D cp{};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = CUdeviceptr(uintptr_t(f->data[0]));
    cp.srcPitch = size_t(f->linesize[0]);
    cp.dstDevice = imports_[slot].ptr;
    cp.dstPitch = size_t(d.width);
    cp.WidthInBytes = size_t(d.width);
    cp.Height = size_t(d.height);
    if (cuMemcpy2DAsync(&cp, cuda_.stream()) != CUDA_SUCCESS) return;
    cp.srcDevice = CUdeviceptr(uintptr_t(f->data[1]));
    cp.srcPitch = size_t(f->linesize[1]);
    cp.dstDevice = imports_[slot].ptr + size_t(d.width) * d.height;
    cp.Height = size_t(d.height / 2);
    if (cuMemcpy2DAsync(&cp, cuda_.stream()) != CUDA_SUCCESS) return;
    if (cuStreamSynchronize(cuda_.stream()) != CUDA_SUCCESS) return;

    const uint64_t v = ring_->submit(slot);
    auto frame = std::make_shared<const gpu::GpuFrame>(ring_, slot, v);
    mailbox_.publish(frame);
    if (syncFrames_ >= 0) {
        const int64_t arrNs = MediaClock::nowNs();
        const int64_t tsNs = 1'000'000'000LL * d.fpsD / d.fpsN;
        const int64_t bet = f->best_effort_timestamp;
        int64_t ptsNs;
        if (!ptsSynth_ && bet == AV_NOPTS_VALUE) {
            ptsSynth_ = true;
            Stats::counter("in" + std::to_string(index_) + ".sync.ptsSynth")
                .add();
        }
        if (ptsSynth_) {
            // Arrival-anchored grid; re-base across gaps so the scheduler
            // sees them as pts discontinuities (resync). Poorer than real
            // pts for SRT (decoder emission is bursty), fallback only.
            if (synthK_ == 0 || arrNs - synthLastArrNs_ > 500'000'000LL) {
                synthBaseNs_ = arrNs;
                synthK_ = 0;
            }
            ptsNs = synthBaseNs_ + synthK_ * tsNs;
            ++synthK_;
            synthLastArrNs_ = arrNs;
        } else {
            // TS 90 kHz pts carry the sender's clean cadence even when the
            // decoder emits in clumps -- exactly what the scheduler wants.
            ptsNs = av_rescale_q(bet, ic_->streams[vidIdx_]->time_base,
                                 AVRational{1, 1'000'000'000});
        }
        if (mediaMode_ && mediaFirstPtsNs_ != INT64_MIN) {
            const int speedPermille =
                std::max(1, currentMediaItem().speedPermille);
            ptsNs = mediaFirstPtsNs_ +
                    (ptsNs - mediaFirstPtsNs_) * 1000 / speedPermille;
        }
        if (!feed_.push({frame, ++pubSeq_, ptsNs, arrNs, !ptsSynth_}))
            Stats::counter("in" + std::to_string(index_) + ".sync.feedDrops")
                .add();
    }
    frames_.fetch_add(1, std::memory_order_relaxed);
    frameCtr.add();
}

bool SrtInput::paceTimestamp(int streamIndex, int64_t timestamp,
                             std::stop_token stop) {
    if (!mediaMode_ || timestamp == AV_NOPTS_VALUE) return true;
    const int64_t ptsNs =
        av_rescale_q(timestamp, ic_->streams[streamIndex]->time_base,
                     AVRational{1, 1'000'000'000});
    const int64_t now = MediaClock::nowNs();
    if (mediaFirstPtsNs_ == INT64_MIN) {
        mediaFirstPtsNs_ = ptsNs;
        mediaStartMonoNs_ = now;
    }
    const int64_t relativeNs = std::max<int64_t>(0, ptsNs - mediaFirstPtsNs_);
    const int64_t target =
        mediaStartMonoNs_ +
        media::playlistWallDurationNs(currentMediaItem(), relativeNs);
    while (!stop.stop_requested() &&
           !mediaRestart_.load(std::memory_order_acquire) &&
           mediaSelect_.load(std::memory_order_acquire) < 0 &&
           mediaPlaying_.load(std::memory_order_acquire)) {
        const int64_t remaining = target - MediaClock::nowNs();
        if (remaining <= 0) break;
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(std::min<int64_t>(remaining, 5'000'000)));
    }
    return !stop.stop_requested() &&
           !mediaRestart_.load(std::memory_order_acquire) &&
           mediaSelect_.load(std::memory_order_acquire) < 0 &&
           mediaPlaying_.load(std::memory_order_acquire);
}

void SrtInput::run(std::stop_token st) {
    auto& reconnCtr = Stats::counter("in" + std::to_string(index_) + ".reconnects");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool everConnected = false;

    while (!st.stop_requested()) {
        if (mediaMode_) {
            if (const int selected =
                    mediaSelect_.exchange(-1, std::memory_order_acq_rel);
                selected >= 0) {
                mediaIndex_.store(selected, std::memory_order_relaxed);
                closeStream();
                mediaAtEnd_.store(false, std::memory_order_relaxed);
            }
            if (mediaRestart_.exchange(false, std::memory_order_acq_rel)) {
                closeStream();
                mediaAtEnd_.store(false, std::memory_order_relaxed);
            }

            const bool playing =
                mediaPlaying_.load(std::memory_order_acquire);
            if (!playing) {
                if (mediaWasPlaying_) {
                    mediaPauseMonoNs_ = MediaClock::nowNs();
                    mediaWasPlaying_ = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (!mediaWasPlaying_) {
                if (mediaPauseMonoNs_ && mediaStartMonoNs_)
                    mediaStartMonoNs_ +=
                        MediaClock::nowNs() - mediaPauseMonoNs_;
                mediaWasPlaying_ = true;
            }
        }

        if (!ic_) {
            if (!openStream()) {
                closeStream();
                if (mediaMode_) advanceMedia();
                for (int i = 0; i < 10 && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (everConnected && !mediaMode_) reconnCtr.add();
            everConnected = true;
        }

        const int r = av_read_frame(ic_, pkt);
        if (r < 0) {
            if (mediaMode_) {
                advanceMedia();
            } else {
                MOO_LOGW("in%d(srt): read error/eof (%d); reconnecting",
                         index_, r);
                closeStream();
            }
            continue;
        }
        if (pkt->stream_index == vidIdx_) {
            if (avcodec_send_packet(dec_, pkt) >= 0) {
                while (avcodec_receive_frame(dec_, frame) >= 0) {
                    const auto item = currentMediaItem();
                    const int64_t position = mediaTimestampMs(
                        vidIdx_, frame->best_effort_timestamp);
                    if (position >= 0 &&
                        media::playlistBeforeIn(item, position)) {
                        av_frame_unref(frame);
                        continue;
                    }
                    if (position >= 0 &&
                        media::playlistAtOrPastOut(item, position)) {
                        mediaPositionMs_.store(item.outMs,
                                               std::memory_order_relaxed);
                        mediaClipDone_ = true;
                        av_frame_unref(frame);
                        break;
                    }
                    if (!paceTimestamp(vidIdx_, frame->best_effort_timestamp,
                                       st)) {
                        av_frame_unref(frame);
                        break;
                    }
                    handleFrame(frame);
                    if (mediaMode_ && position >= 0)
                        mediaPositionMs_.store(position,
                                               std::memory_order_relaxed);
                    av_frame_unref(frame);
                }
            }
        } else if (adec_ && pkt->stream_index == audIdx_) {
            if (avcodec_send_packet(adec_, pkt) >= 0) {
                while (avcodec_receive_frame(adec_, frame) >= 0) {
                    const auto item = currentMediaItem();
                    const int64_t position = mediaTimestampMs(
                        audIdx_, frame->best_effort_timestamp);
                    if (position >= 0 &&
                        (media::playlistBeforeIn(item, position) ||
                         media::playlistAtOrPastOut(item, position))) {
                        av_frame_unref(frame);
                        continue;
                    }
                    if (!paceTimestamp(audIdx_, frame->best_effort_timestamp,
                                       st)) {
                        av_frame_unref(frame);
                        break;
                    }
                    handleAudioFrame(frame);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
        if (mediaClipDone_) advanceMedia();
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}

}  // namespace moo
