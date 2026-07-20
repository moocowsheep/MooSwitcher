/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "media/AacEncoder.h"

TEST_CASE("aac encoder: accumulates 1024-sample frames with sample-index PTS") {
    moo::media::AacEncoder enc;
    REQUIRE(enc.open(48000, 128000));
    REQUIRE(enc.codecCtx()->frame_size == 1024);

    std::vector<AVPacket*> pkts;
    std::vector<float> chunk(480 * 2);
    for (int f = 0; f < 480; ++f) {
        const float v = 0.4f * std::sin(2.f * float(M_PI) * float(f % 48) / 48.f);
        chunk[size_t(2 * f)] = v;
        chunk[size_t(2 * f + 1)] = v;
    }

    // 20 chunks of 480 = 9600 samples = 9 full AAC frames + 384 leftover.
    for (int c = 0; c < 20; ++c)
        REQUIRE(enc.encode(chunk.data(), 480, int64_t(c) * 480, pkts));
    REQUIRE(enc.drain(pkts));

    REQUIRE(pkts.size() >= 8);
    for (size_t i = 1; i < pkts.size(); ++i)
        REQUIRE(pkts[i]->pts - pkts[i - 1]->pts == 1024);
    for (auto* p : pkts) {
        REQUIRE(p->size > 0);
        av_packet_free(&p);
    }
}

TEST_CASE("aac encoder: resyncs PTS at empty accumulator after a gap") {
    moo::media::AacEncoder enc;
    REQUIRE(enc.open(48000, 128000));

    std::vector<AVPacket*> pkts;
    std::vector<float> chunk(1024 * 2, 0.1f);  // exactly one frame per push

    REQUIRE(enc.encode(chunk.data(), 1024, 0, pkts));
    // 100ms dropout: the next push starts at sample 9600, not 1024.
    REQUIRE(enc.encode(chunk.data(), 1024, 9600, pkts));
    REQUIRE(enc.encode(chunk.data(), 1024, 10624, pkts));
    REQUIRE(enc.drain(pkts));

    // Priming packet (pts = -initial_padding), then the three frames with
    // the gap preserved between the first and second.
    REQUIRE(pkts.size() == 4);
    REQUIRE(pkts[1]->pts == 0);
    REQUIRE(pkts[2]->pts - pkts[1]->pts == 9600);
    REQUIRE(pkts[3]->pts - pkts[2]->pts == 1024);
    for (auto* p : pkts) av_packet_free(&p);
}
