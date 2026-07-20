/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
