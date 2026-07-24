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
// The direct NVENC backend: registered-in-place NV12 in, decodable HEVC out.
// Skips without a GPU/CUDA/NVENC.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"
#include "media/FfmpegNvenc.h"
#include "media/NvencDirect.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

using namespace moo;

namespace {

constexpr int kW = 640, kH = 360, kFrames = 20;
const VideoFormatDesc kShow{kW, kH, 60000, 1001, PixFmt::NV12};
// Preset pinned so the test measures the encoder, not the Auto policy.
const media::EncoderConfig kDirectCfg{media::EncoderBackend::Direct,
                                      media::EncoderPreset::P4, 4000, false};

// Tight-pitch NV12 with a block that moves frame to frame, so the encoder has
// real motion to code rather than a run of skip frames.
std::vector<uint8_t> nv12Frame(int n) {
    std::vector<uint8_t> f(size_t(kW) * kH * 3 / 2, 128);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            f[size_t(y) * kW + x] = uint8_t(16 + ((x + y) >> 2) % 200);
    const int bx = (n * 17) % (kW - 64), by = (n * 11) % (kH - 64);
    for (int y = by; y < by + 64; ++y)
        memset(&f[size_t(y) * kW + bx], 235, 64);
    return f;
}

// Decodes an elementary stream and returns the frame count, or -1 on failure.
int decodeFrames(const std::vector<AVPacket*>& packets,
                 const AVCodecParameters* par, int& outW, int& outH) {
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!dec) return -1;
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx) return -1;
    if (par && par->extradata_size > 0) {
        ctx->extradata = static_cast<uint8_t*>(
            av_mallocz(size_t(par->extradata_size) + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(ctx->extradata, par->extradata, size_t(par->extradata_size));
        ctx->extradata_size = par->extradata_size;
    }
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return -1;
    }

    AVFrame* frame = av_frame_alloc();
    int count = 0;
    auto receive = [&] {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            outW = frame->width;
            outH = frame->height;
            ++count;
        }
    };
    for (AVPacket* pkt : packets) {
        if (avcodec_send_packet(ctx, pkt) < 0) break;
        receive();
    }
    avcodec_send_packet(ctx, nullptr);
    receive();

    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return count;
}

// Owns the GPU/CUDA bring-up plus one device-side NV12 staging buffer.
struct CudaFixture {
    gpu::VkEngine eng;
    media::CudaCtx cuda;
    CUdeviceptr buf = 0;

    ~CudaFixture() {
        if (buf) cuMemFree(buf);
    }

    void upload(int n) {
        const std::vector<uint8_t> host = nv12Frame(n);
        REQUIRE(cuMemcpyHtoD(buf, host.data(), host.size()) == CUDA_SUCCESS);
    }
};

// Returns nullptr (after SKIPping) when the box has no usable GPU stack.
std::unique_ptr<CudaFixture> makeFixture() {
    auto fx = std::make_unique<CudaFixture>();
    if (!fx->eng.init(false)) return nullptr;
    if (!fx->cuda.init(fx->eng.deviceUuid())) return nullptr;
    fx->cuda.makeCurrent();
    if (cuMemAlloc(&fx->buf, size_t(kW) * kH * 3 / 2) != CUDA_SUCCESS)
        return nullptr;
    return fx;
}

void freeAll(std::vector<AVPacket*>& packets) {
    for (AVPacket* pkt : packets) av_packet_free(&pkt);
    packets.clear();
}

}  // namespace

TEST_CASE("Auto encoder preset picks speed only where the tick needs it") {
    using media::EncoderPreset;
    auto autoFor = [](int w, int h) {
        return media::resolveEncoderPreset(EncoderPreset::Auto,
                                           VideoFormatDesc{w, h, 60000, 1001});
    };
    CHECK(autoFor(1920, 1080) == EncoderPreset::P4);
    CHECK(autoFor(3840, 2160) == EncoderPreset::P4);
    CHECK(autoFor(7680, 4320) == EncoderPreset::P2);
    // An explicit preset is never second-guessed.
    CHECK(media::resolveEncoderPreset(EncoderPreset::P7,
                                      VideoFormatDesc{7680, 4320, 60000, 1001}) ==
          EncoderPreset::P7);

    EncoderPreset parsed = EncoderPreset::P4;
    REQUIRE(media::parseEncoderPreset("p1", parsed));
    CHECK(parsed == EncoderPreset::P1);
    REQUIRE(media::parseEncoderPreset("auto", parsed));
    CHECK(parsed == EncoderPreset::Auto);
    CHECK_FALSE(media::parseEncoderPreset("p8", parsed));
    CHECK_FALSE(media::parseEncoderPreset("fast", parsed));
    CHECK(parsed == EncoderPreset::Auto);  // untouched on failure
}

TEST_CASE("NvencDirect encodes registered CUDA NV12 into decodable HEVC") {
    auto fx = makeFixture();
    if (!fx) SKIP("no Vulkan/CUDA device");

    media::NvencDirect enc;
    if (!enc.open(fx->cuda, kShow, kDirectCfg))
        SKIP("no NVENC session (driver or headers too old)");

    std::vector<AVPacket*> packets;
    for (int n = 0; n < kFrames; ++n) {
        fx->upload(n);
        REQUIRE(enc.encode(fx->buf, n, packets));
        // Synchronous session: one packet per picture, no reordering delay.
        REQUIRE(int(packets.size()) == n + 1);
        REQUIRE(packets.back()->pts == n);
        REQUIRE(packets.back()->dts == n);
    }
    REQUIRE(enc.drain(packets));
    REQUIRE((packets.front()->flags & AV_PKT_FLAG_KEY) != 0);

    // In-band SPS/PPS: the stream decodes with no extradata at all.
    int w = 0, h = 0;
    REQUIRE(decodeFrames(packets, nullptr, w, h) == kFrames);
    REQUIRE(w == kW);
    REQUIRE(h == kH);
    freeAll(packets);
}

TEST_CASE("NvencDirect exports codec parameters for a global-header muxer") {
    auto fx = makeFixture();
    if (!fx) SKIP("no Vulkan/CUDA device");

    media::NvencDirect enc;
    media::EncoderConfig globalHeaderCfg = kDirectCfg;
    globalHeaderCfg.globalHeader = true;
    if (!enc.open(fx->cuda, kShow, globalHeaderCfg))
        SKIP("no NVENC session (driver or headers too old)");

    AVCodecParameters* par = avcodec_parameters_alloc();
    REQUIRE(enc.fillCodecpar(par));
    CHECK(par->codec_id == AV_CODEC_ID_HEVC);
    CHECK(par->width == kW);
    CHECK(par->height == kH);
    REQUIRE(par->extradata_size > 0);

    std::vector<AVPacket*> packets;
    for (int n = 0; n < kFrames; ++n) {
        fx->upload(n);
        REQUIRE(enc.encode(fx->buf, n, packets));
    }
    REQUIRE(enc.drain(packets));

    int w = 0, h = 0;
    REQUIRE(decodeFrames(packets, par, w, h) == kFrames);
    CHECK(w == kW);
    CHECK(h == kH);
    freeAll(packets);
    avcodec_parameters_free(&par);
}

TEST_CASE("NVENC backends agree frame for frame") {
    auto fx = makeFixture();
    if (!fx) SKIP("no Vulkan/CUDA device");

    media::FfmpegNvenc viaFfmpeg;
    media::NvencDirect direct;
    if (!viaFfmpeg.open(fx->cuda, kShow, kDirectCfg)) SKIP("no hevc_nvenc");
    if (!direct.open(fx->cuda, kShow, kDirectCfg)) SKIP("no NVENC session");

    CHECK(viaFfmpeg.timeBase().num == direct.timeBase().num);
    CHECK(viaFfmpeg.timeBase().den == direct.timeBase().den);

    std::vector<AVPacket*> a, b;
    for (int n = 0; n < kFrames; ++n) {
        fx->upload(n);
        REQUIRE(viaFfmpeg.encode(fx->buf, n, a));
        REQUIRE(direct.encode(fx->buf, n, b));
    }
    REQUIRE(viaFfmpeg.drain(a));
    REQUIRE(direct.drain(b));

    int wa = 0, ha = 0, wb = 0, hb = 0;
    REQUIRE(decodeFrames(a, nullptr, wa, ha) == kFrames);
    REQUIRE(decodeFrames(b, nullptr, wb, hb) == kFrames);
    CHECK(wa == wb);
    CHECK(ha == hb);
    freeAll(a);
    freeAll(b);
}
