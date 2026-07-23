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

#include "media/FfmpegNvenc.h"

#include <algorithm>

#include "core/Log.h"

extern "C" {
#include <libavutil/hwcontext_cuda.h>
#include <libavutil/opt.h>
}

namespace moo::media {

bool FfmpegNvenc::open(CudaCtx& cuda, const VideoFormatDesc& show,
                       int bitrateKbps, bool globalHeader) {
    cuda_ = &cuda;
    w_ = show.width;
    h_ = show.height;
    const double fps = double(show.fpsN) / double(show.fpsD);
    if (bitrateKbps <= 0)
        bitrateKbps = std::max(8000, int(double(w_) * h_ * fps * 0.04 / 1000.0));

    // Device ctx wrapping OUR retained primary context.
    hwDev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if (!hwDev_) return false;
    auto* dctx = reinterpret_cast<AVHWDeviceContext*>(hwDev_->data);
    auto* cuctx = static_cast<AVCUDADeviceContext*>(dctx->hwctx);
    cuctx->cuda_ctx = cuda.ctx();
    if (av_hwdevice_ctx_init(hwDev_) < 0) {
        MOO_LOGE("nvenc: av_hwdevice_ctx_init failed");
        return false;
    }

    hwFrames_ = av_hwframe_ctx_alloc(hwDev_);
    if (!hwFrames_) return false;
    auto* fctx = reinterpret_cast<AVHWFramesContext*>(hwFrames_->data);
    fctx->format = AV_PIX_FMT_CUDA;
    fctx->sw_format = AV_PIX_FMT_NV12;
    fctx->width = w_;
    fctx->height = h_;
    fctx->initial_pool_size = 4;
    if (av_hwframe_ctx_init(hwFrames_) < 0) {
        MOO_LOGE("nvenc: av_hwframe_ctx_init failed");
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("hevc_nvenc");
    if (!codec) {
        MOO_LOGE("nvenc: hevc_nvenc not available in this FFmpeg");
        return false;
    }
    enc_ = avcodec_alloc_context3(codec);
    enc_->width = w_;
    enc_->height = h_;
    enc_->time_base = {int(show.fpsD), int(show.fpsN)};
    enc_->framerate = {int(show.fpsN), int(show.fpsD)};
    enc_->pix_fmt = AV_PIX_FMT_CUDA;
    if (globalHeader) enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    enc_->hw_frames_ctx = av_buffer_ref(hwFrames_);
    enc_->max_b_frames = 0;
    enc_->gop_size = std::max(1, int(fps * 2));  // IDR every ~2s
    enc_->bit_rate = int64_t(bitrateKbps) * 1000;
    enc_->rc_max_rate = enc_->bit_rate;
    enc_->rc_buffer_size = int(enc_->bit_rate * show.fpsD / show.fpsN);  // 1 frame
    av_opt_set(enc_->priv_data, "preset", "p4", 0);
    av_opt_set(enc_->priv_data, "tune", "ull", 0);
    av_opt_set(enc_->priv_data, "rc", "cbr", 0);
    av_opt_set_int(enc_->priv_data, "delay", 0, 0);

    if (const int r = avcodec_open2(enc_, codec, nullptr); r < 0) {
        char buf[128];
        av_strerror(r, buf, sizeof buf);
        MOO_LOGE("nvenc: avcodec_open2 failed: %s", buf);
        avcodec_free_context(&enc_);
        return false;
    }
    frame_ = av_frame_alloc();
    MOO_LOGI("nvenc: hevc %dx%d @ %.3f fps, %d kbps CBR (p4/ull)", w_, h_, fps,
             bitrateKbps);
    return true;
}

void FfmpegNvenc::close() {
    if (frame_) av_frame_free(&frame_);
    if (enc_) avcodec_free_context(&enc_);
    if (hwFrames_) av_buffer_unref(&hwFrames_);
    if (hwDev_) av_buffer_unref(&hwDev_);
}

bool FfmpegNvenc::receiveAll(std::vector<AVPacket*>& out) {
    while (true) {
        AVPacket* pkt = av_packet_alloc();
        const int r = avcodec_receive_packet(enc_, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
            av_packet_free(&pkt);
            return true;
        }
        if (r < 0) {
            av_packet_free(&pkt);
            MOO_LOGE("nvenc: receive_packet error %d", r);
            return false;
        }
        out.push_back(pkt);
    }
}

bool FfmpegNvenc::encode(CUdeviceptr src, int64_t pts,
                         std::vector<AVPacket*>& out) {
    cuda_->makeCurrent();
    av_frame_unref(frame_);
    if (av_hwframe_get_buffer(hwFrames_, frame_, 0) < 0) {
        MOO_LOGE("nvenc: hwframe_get_buffer failed");
        return false;
    }

    CUDA_MEMCPY2D cp{};
    cp.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.dstMemoryType = CU_MEMORYTYPE_DEVICE;
    cp.srcDevice = src;
    cp.srcPitch = size_t(w_);
    cp.dstDevice = CUdeviceptr(uintptr_t(frame_->data[0]));
    cp.dstPitch = size_t(frame_->linesize[0]);
    cp.WidthInBytes = size_t(w_);
    cp.Height = size_t(h_);
    if (cuMemcpy2DAsync(&cp, cuda_->stream()) != CUDA_SUCCESS) return false;

    cp.srcDevice = src + size_t(w_) * h_;
    cp.dstDevice = CUdeviceptr(uintptr_t(frame_->data[1]));
    cp.dstPitch = size_t(frame_->linesize[1]);
    cp.Height = size_t(h_ / 2);
    if (cuMemcpy2DAsync(&cp, cuda_->stream()) != CUDA_SUCCESS) return false;
    if (cuStreamSynchronize(cuda_->stream()) != CUDA_SUCCESS) return false;
    // src is reusable from here.

    frame_->pts = pts;
    if (const int r = avcodec_send_frame(enc_, frame_); r < 0) {
        MOO_LOGE("nvenc: send_frame error %d", r);
        return false;
    }
    return receiveAll(out);
}

bool FfmpegNvenc::drain(std::vector<AVPacket*>& out) {
    if (!enc_) return true;
    avcodec_send_frame(enc_, nullptr);
    return receiveAll(out);
}

}  // namespace moo::media
