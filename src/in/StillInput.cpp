/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "in/StillInput.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "core/Log.h"
#include "core/Stats.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace moo {

StillInput::StillInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue,
                       std::string path, int index, int64_t fpsN,
                       int64_t fpsD)
    : eng_(eng), queue_(uploadQueue), path_(std::move(path)), index_(index),
      fpsN_(fpsN), fpsD_(fpsD) {
    thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

StillInput::~StillInput() {
    stopFlag_.store(true, std::memory_order_release);
    thread_ = {};
}

IInputSource::Status StillInput::status() const {
    Status state;
    state.connected = connected_.load(std::memory_order_relaxed);
    state.frames = frames_.load(std::memory_order_relaxed);
    std::lock_guard lock(descM_);
    state.desc = desc_;
    return state;
}

int StillInput::interruptCb(void* opaque) {
    return static_cast<StillInput*>(opaque)->stopFlag_.load(
        std::memory_order_relaxed);
}

bool StillInput::publishFrame(const AVFrame* frame) {
    if (!frame || frame->width < 2 || frame->height < 1 ||
        frame->format == AV_PIX_FMT_NONE)
        return false;

    // Packed UYVY requires pairs of luma pixels. Scale an odd source width up
    // by one pixel instead of silently cropping its right edge.
    const int width = (frame->width + 1) & ~1;
    const int height = frame->height;
    if (width != frame->width)
        MOO_LOGW("in%d(still): odd width %d scaled to %d", index_,
                 frame->width, width);

    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    SwsContext* toRgba =
        sws_getContext(frame->width, frame->height,
                       AVPixelFormat(frame->format), width, height,
                       AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr,
                       nullptr);
    if (!toRgba) return false;
    uint8_t* rgbaPlanes[] = {rgba.data(), nullptr, nullptr, nullptr};
    const int rgbaStrides[] = {width * 4, 0, 0, 0};
    const int rgbaRows =
        sws_scale(toRgba, frame->data, frame->linesize, 0, frame->height,
                  rgbaPlanes, rgbaStrides);
    sws_freeContext(toRgba);
    if (rgbaRows != height) return false;

    bool hasAlpha = false;
    for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel) {
        if (rgba[pixel * 4 + 3] != 0xff) {
            hasAlpha = true;
            break;
        }
    }

    VideoFormatDesc desc;
    desc.width = width;
    desc.height = height;
    desc.fpsN = fpsN_ > 0 ? fpsN_ : 60000;
    desc.fpsD = fpsD_ > 0 ? fpsD_ : 1001;
    desc.pixfmt = hasAlpha ? PixFmt::UYVA8_4224 : PixFmt::UYVY8_422;
    desc.colorimetry = VideoFormatDesc::colorimetryForHeight(height);

    auto nextRing =
        std::make_shared<gpu::UploadRing>(eng_, desc, queue_, 1);
    const int slot = nextRing->acquire();
    if (slot < 0) return false;
    uint8_t* staging = nextRing->stagingPtr(slot);

    SwsContext* toUyvy =
        sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height,
                       AV_PIX_FMT_UYVY422, SWS_BILINEAR, nullptr, nullptr,
                       nullptr);
    if (!toUyvy) return false;
    const int colorSpace = desc.colorimetry == Colorimetry::BT601
                               ? SWS_CS_ITU601
                               : SWS_CS_ITU709;
    const int* coefficients = sws_getCoefficients(colorSpace);
    if (coefficients)
        sws_setColorspaceDetails(toUyvy, coefficients, 1, coefficients, 0,
                                 0, 1 << 16, 1 << 16);
    const uint8_t* sourcePlanes[] = {rgba.data(), nullptr, nullptr, nullptr};
    const int sourceStrides[] = {width * 4, 0, 0, 0};
    uint8_t* uyvyPlanes[] = {staging, nullptr, nullptr, nullptr};
    const int uyvyStrides[] = {width * 2, 0, 0, 0};
    const int uyvyRows =
        sws_scale(toUyvy, sourcePlanes, sourceStrides, 0, height, uyvyPlanes,
                  uyvyStrides);
    sws_freeContext(toUyvy);
    if (uyvyRows != height) return false;

    if (hasAlpha) {
        uint8_t* alpha = staging + desc.alphaOffset();
        for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel)
            alpha[pixel] = rgba[pixel * 4 + 3];
    }

    const uint64_t upload = nextRing->submit(slot);
    auto gpuFrame = std::make_shared<const gpu::GpuFrame>(
        nextRing, slot, upload, /*premultiplied=*/false);
    ring_ = std::move(nextRing);
    mailbox_.publish(std::move(gpuFrame));
    {
        std::lock_guard lock(descM_);
        desc_ = desc;
    }
    connected_.store(true, std::memory_order_release);
    frames_.store(1, std::memory_order_relaxed);
    Stats::counter("in" + std::to_string(index_) + ".frames").add();
    MOO_LOGI("in%d(still): opened '%s', %dx%d%s", index_, path_.c_str(),
             width, height, hasAlpha ? " +alpha" : "");
    return true;
}

void StillInput::run(std::stop_token stop) {
    AVFormatContext* input = avformat_alloc_context();
    AVCodecContext* decoder = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int videoIndex = -1;
    bool published = false;

    const auto cleanup = [&] {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        if (input) avformat_close_input(&input);
    };

    if (!input || !packet || !frame) {
        MOO_LOGE("in%d(still): FFmpeg allocation failed", index_);
        cleanup();
        return;
    }
    input->interrupt_callback = {&StillInput::interruptCb, this};
    if (avformat_open_input(&input, path_.c_str(), nullptr, nullptr) < 0 ||
        avformat_find_stream_info(input, nullptr) < 0) {
        MOO_LOGE("in%d(still): could not open '%s'", index_, path_.c_str());
        cleanup();
        return;
    }
    videoIndex =
        av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoIndex < 0) {
        MOO_LOGE("in%d(still): no decodable image in '%s'", index_,
                 path_.c_str());
        cleanup();
        return;
    }
    const AVCodecParameters* parameters =
        input->streams[videoIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
    decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!decoder || avcodec_parameters_to_context(decoder, parameters) < 0 ||
        avcodec_open2(decoder, codec, nullptr) < 0) {
        MOO_LOGE("in%d(still): decoder init failed for '%s'", index_,
                 path_.c_str());
        cleanup();
        return;
    }

    const auto receive = [&] {
        while (!stop.stop_requested() &&
               avcodec_receive_frame(decoder, frame) >= 0) {
            published = publishFrame(frame);
            av_frame_unref(frame);
            if (published) return;
        }
    };

    while (!published && !stop.stop_requested()) {
        const int read = av_read_frame(input, packet);
        if (read < 0) {
            avcodec_send_packet(decoder, nullptr);
            receive();
            break;
        }
        if (packet->stream_index == videoIndex &&
            avcodec_send_packet(decoder, packet) >= 0)
            receive();
        av_packet_unref(packet);
    }
    if (!published && !stop.stop_requested())
        MOO_LOGE("in%d(still): image decode failed for '%s'", index_,
                 path_.c_str());
    cleanup();
}

}  // namespace moo
