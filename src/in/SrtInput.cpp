#include "in/SrtInput.h"

#include <mutex>

#include "core/Log.h"
#include "core/Stats.h"

extern "C" {
#include <libavutil/hwcontext_cuda.h>
}

namespace moo {

SrtInput::SrtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                   media::CudaCtx& cuda, std::string url, int index)
    : eng_(eng), queue_(uploadQueue), cuda_(cuda), url_(std::move(url)),
      index_(index) {
    static std::once_flag netInit;
    std::call_once(netInit, [] { avformat_network_init(); });

    hwDev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (hwDev_) {
        auto* dctx = reinterpret_cast<AVHWDeviceContext*>(hwDev_->data);
        static_cast<AVCUDADeviceContext*>(dctx->hwctx)->cuda_ctx = cuda_.ctx();
        if (av_hwdevice_ctx_init(hwDev_) < 0) av_buffer_unref(&hwDev_);
    }
    if (!hwDev_) {
        MOO_LOGE("in%d(srt): CUDA hwdevice init failed", index_);
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

int SrtInput::interruptCb(void* opaque) {
    return static_cast<SrtInput*>(opaque)->stopFlag_.load(std::memory_order_relaxed);
}

AVPixelFormat SrtInput::pickCuda(AVCodecContext*, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_CUDA) return *p;
    return fmts[0];
}

bool SrtInput::openStream() {
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

    connected_.store(true, std::memory_order_relaxed);
    MOO_LOGI("in%d(srt): connected, %s %dx%d", index_, codec->name, par->width,
             par->height);
    return true;
}

void SrtInput::closeStream() {
    if (dec_) avcodec_free_context(&dec_);
    if (ic_) avformat_close_input(&ic_);
    vidIdx_ = -1;
    connected_.store(false, std::memory_order_relaxed);
}

void SrtInput::handleFrame(AVFrame* f) {
    auto& dropCtr = Stats::counter("in" + std::to_string(index_) + ".drops");
    auto& frameCtr = Stats::counter("in" + std::to_string(index_) + ".frames");

    if (f->format != AV_PIX_FMT_CUDA) return;

    VideoFormatDesc d;
    d.width = f->width;
    d.height = f->height;
    d.pixfmt = PixFmt::NV12;
    const AVRational fr = ic_->streams[vidIdx_]->avg_frame_rate;
    if (fr.num > 0 && fr.den > 0) {
        d.fpsN = fr.num;
        d.fpsD = fr.den;
    }
    d.colorimetry = VideoFormatDesc::colorimetryForHeight(f->height);

    if (!ring_ || !(ring_->desc() == d)) {
        for (auto& im : imports_) cuda_.release(im);
        ring_ = std::make_shared<gpu::Nv12Ring>(eng_, d, queue_);
        for (int s = 0; s < gpu::Nv12Ring::kSlots; ++s) {
            const int fd = ring_->exportStagingFd(s);
            if (fd < 0 || !cuda_.importVkFd(fd, ring_->stagingBytes(), imports_[s])) {
                MOO_LOGE("in%d(srt): staging import failed", index_);
                ring_.reset();
                return;
            }
        }
        std::lock_guard lk(descM_);
        desc_ = d;
        MOO_LOGI("in%d(srt): format %dx%d @ %lld/%lld", index_, d.width, d.height,
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
    mailbox_.publish(std::make_shared<const gpu::GpuFrame>(ring_, slot, v));
    frames_.fetch_add(1, std::memory_order_relaxed);
    frameCtr.add();
}

void SrtInput::run(std::stop_token st) {
    auto& reconnCtr = Stats::counter("in" + std::to_string(index_) + ".reconnects");
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool everConnected = false;

    while (!st.stop_requested()) {
        if (!ic_) {
            if (!openStream()) {
                closeStream();
                for (int i = 0; i < 10 && !st.stop_requested(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (everConnected) reconnCtr.add();
            everConnected = true;
        }

        const int r = av_read_frame(ic_, pkt);
        if (r < 0) {
            MOO_LOGW("in%d(srt): read error/eof (%d); reconnecting", index_, r);
            closeStream();
            continue;
        }
        if (pkt->stream_index == vidIdx_) {
            if (avcodec_send_packet(dec_, pkt) >= 0) {
                while (avcodec_receive_frame(dec_, frame) >= 0) {
                    handleFrame(frame);
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
