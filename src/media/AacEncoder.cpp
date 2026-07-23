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

#include "media/AacEncoder.h"

#include <algorithm>

#include "core/Log.h"

namespace moo::media {

bool AacEncoder::open(int sampleRate, int bitrateBps) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        MOO_LOGE("aac: no encoder in this FFmpeg");
        return false;
    }
    enc_ = avcodec_alloc_context3(codec);
    if (!enc_) return false;
    enc_->sample_rate = sampleRate;
    enc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&enc_->ch_layout, 2);
    enc_->bit_rate = bitrateBps;
    enc_->time_base = {1, sampleRate};
    enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // mpegts writes ADTS from this
    if (avcodec_open2(enc_, codec, nullptr) < 0) {
        MOO_LOGE("aac: open failed");
        close();
        return false;
    }
    frame_ = av_frame_alloc();
    frame_->format = enc_->sample_fmt;
    frame_->sample_rate = enc_->sample_rate;
    av_channel_layout_copy(&frame_->ch_layout, &enc_->ch_layout);
    frame_->nb_samples = enc_->frame_size;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        close();
        return false;
    }
    return true;
}

void AacEncoder::close() {
    if (frame_) av_frame_free(&frame_);
    if (enc_) avcodec_free_context(&enc_);
    fill_ = 0;
}

bool AacEncoder::receiveAll(std::vector<AVPacket*>& out) {
    AVPacket* p = av_packet_alloc();
    while (true) {
        const int r = avcodec_receive_packet(enc_, p);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) {
            av_packet_free(&p);
            return false;
        }
        out.push_back(p);
        p = av_packet_alloc();
    }
    av_packet_free(&p);
    return true;
}

bool AacEncoder::encode(const float* lr, int frames, int64_t firstSample,
                        std::vector<AVPacket*>& out) {
    if (!enc_) return false;
    int consumed = 0;
    while (consumed < frames) {
        if (fill_ == 0) {
            framePts_ = firstSample + consumed;
            if (av_frame_make_writable(frame_) < 0) return false;
        }
        const int take = std::min(frames - consumed, enc_->frame_size - fill_);
        float* l = reinterpret_cast<float*>(frame_->data[0]) + fill_;
        float* r = reinterpret_cast<float*>(frame_->data[1]) + fill_;
        for (int f = 0; f < take; ++f) {
            l[f] = lr[2 * (consumed + f)];
            r[f] = lr[2 * (consumed + f) + 1];
        }
        fill_ += take;
        consumed += take;
        if (fill_ == enc_->frame_size) {
            frame_->pts = framePts_;
            fill_ = 0;
            if (avcodec_send_frame(enc_, frame_) < 0) return false;
            if (!receiveAll(out)) return false;
        }
    }
    return true;
}

bool AacEncoder::drain(std::vector<AVPacket*>& out) {
    if (!enc_) return false;
    avcodec_send_frame(enc_, nullptr);
    return receiveAll(out);
}

}  // namespace moo::media
