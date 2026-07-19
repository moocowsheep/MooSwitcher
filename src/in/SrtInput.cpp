#include "in/SrtInput.h"

#include <mutex>

#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

extern "C" {
#include <libavutil/hwcontext_cuda.h>
}

namespace moo {

SrtInput::SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                   media::CudaCtx& cuda, std::string url, int index,
                   int syncFrames, bool mediaMode, bool mediaPlaying,
                   bool mediaLoop)
    : eng_(eng), queue_(uploadQueue), cuda_(cuda), url_(std::move(url)),
      index_(index), mediaMode_(mediaMode), syncFrames_(syncFrames),
      mediaPlaying_(mediaPlaying), mediaLoop_(mediaLoop) {
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
    return {true,
            mediaPlaying_.load(std::memory_order_relaxed),
            mediaLoop_.load(std::memory_order_relaxed),
            mediaAtEnd_.load(std::memory_order_relaxed),
            mediaPositionMs_.load(std::memory_order_relaxed),
            mediaDurationMs_.load(std::memory_order_relaxed)};
}

void SrtInput::setMediaPlaying(bool playing) {
    if (!mediaMode_) return;
    if (playing && mediaAtEnd_.load(std::memory_order_relaxed))
        mediaRestart_.store(true, std::memory_order_release);
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

int SrtInput::interruptCb(void* opaque) {
    return static_cast<SrtInput*>(opaque)->stopFlag_.load(std::memory_order_relaxed);
}

AVPixelFormat SrtInput::pickCuda(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_CUDA) return *p;
    return fmts[0];
}

bool SrtInput::openStream() {
    ptsSynth_ = false;  // re-judge the new stream's timestamps
    synthK_ = 0;
    mediaFirstPtsNs_ = INT64_MIN;
    mediaStartMonoNs_ = 0;
    mediaPauseMonoNs_ = 0;
    mediaWasPlaying_ = false;
    mediaAtEnd_.store(false, std::memory_order_relaxed);
    mediaPositionMs_.store(0, std::memory_order_relaxed);
    ic_ = avformat_alloc_context();
    ic_->interrupt_callback = {&SrtInput::interruptCb, this};
    if (avformat_open_input(&ic_, url_.c_str(), nullptr, nullptr) < 0) return false;
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
    }
    connected_.store(true, std::memory_order_relaxed);
    MOO_LOGI("in%d(%s): opened, %s %dx%d%s", index_,
             mediaMode_ ? "media" : "srt", codec->name, par->width,
             par->height, adec_ ? " + audio" : "");
    return true;
}

void SrtInput::closeStream() {
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

void SrtInput::handleAudioFrame(AVFrame* f) {
    auto* ch = audioSink_.load(std::memory_order_acquire);
    if (!ch || f->nb_samples <= 0) return;

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
    const int64_t target = mediaStartMonoNs_ + relativeNs;
    while (!stop.stop_requested() &&
           !mediaRestart_.load(std::memory_order_acquire) &&
           mediaPlaying_.load(std::memory_order_acquire)) {
        const int64_t remaining = target - MediaClock::nowNs();
        if (remaining <= 0) break;
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(std::min<int64_t>(remaining, 5'000'000)));
    }
    return !stop.stop_requested() &&
           !mediaRestart_.load(std::memory_order_acquire) &&
           mediaPlaying_.load(std::memory_order_acquire);
}

void SrtInput::run(std::stop_token st) {
    auto& reconnCtr = Stats::counter("in" + std::to_string(index_) + ".reconnects");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool everConnected = false;

    while (!st.stop_requested()) {
        if (mediaMode_) {
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
                mediaAtEnd_.store(true, std::memory_order_relaxed);
                if (mediaLoop_.load(std::memory_order_acquire)) {
                    closeStream();
                } else {
                    mediaPlaying_.store(false, std::memory_order_release);
                }
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
                    if (!paceTimestamp(vidIdx_, frame->best_effort_timestamp,
                                       st)) {
                        av_frame_unref(frame);
                        break;
                    }
                    handleFrame(frame);
                    if (mediaMode_ && mediaFirstPtsNs_ != INT64_MIN &&
                        frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        const int64_t ptsNs = av_rescale_q(
                            frame->best_effort_timestamp,
                            ic_->streams[vidIdx_]->time_base,
                            AVRational{1, 1'000'000'000});
                        mediaPositionMs_.store(
                            std::max<int64_t>(0, ptsNs - mediaFirstPtsNs_) /
                                1'000'000,
                            std::memory_order_relaxed);
                    }
                    av_frame_unref(frame);
                }
            }
        } else if (adec_ && pkt->stream_index == audIdx_) {
            if (avcodec_send_packet(adec_, pkt) >= 0) {
                while (avcodec_receive_frame(adec_, frame) >= 0) {
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
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}

}  // namespace moo
