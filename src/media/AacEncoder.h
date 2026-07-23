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

#pragma once
#include <cstdint>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace moo::media {

// AAC-LC via libavcodec for the MPEG-TS mux: stereo FLTP, 1024-sample frames
// accumulated from arbitrary-size pushes (the mixer hands us 480 at a time).
// GLOBAL_HEADER is set so the mpegts muxer can write ADTS from extradata.
// PTS is the absolute sample index of a frame's first sample (1/rate
// timebase, same origin as the video tick PTS): resynced from the caller's
// counter whenever the accumulator is empty, so mixer skips cannot drift
// audio against video for more than one AAC frame.
class AacEncoder {
public:
    ~AacEncoder() { close(); }

    bool open(int sampleRate, int bitrateBps);
    void close();
    bool ok() const { return enc_ != nullptr; }

    const AVCodecContext* codecCtx() const { return enc_; }
    AVRational timeBase() const {
        return enc_ ? enc_->time_base : AVRational{1, 1};
    }

    // Interleaved stereo; firstSample indexes lr[0] on the media timeline.
    // Emitted packets are appended to out (caller frees).
    bool encode(const float* lr, int frames, int64_t firstSample,
                std::vector<AVPacket*>& out);
    bool drain(std::vector<AVPacket*>& out);

private:
    bool receiveAll(std::vector<AVPacket*>& out);

    AVCodecContext* enc_ = nullptr;
    AVFrame* frame_ = nullptr;
    int fill_ = 0;
    int64_t framePts_ = 0;
};

}  // namespace moo::media
