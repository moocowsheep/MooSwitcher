/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/Format.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"
#include "ndi/NdiLib.h"

namespace moo {

// NDI program output: waits for completed pack readbacks, hands the host
// buffer straight to NDIlib_send_send_video_async_v2 (the SDK SpeedHQ-encodes
// from our memory; a slot stays pinned until the *next* async call releases
// it). Drop-to-latest when the encoder falls behind. Audio is embedded on the
// same sender via sendAudio() (the NDI send API is thread-safe, so the mixer
// thread calls it directly while this thread sends video).
class NdiOutput {
public:
    NdiOutput(std::string name, gpu::Compositor& comp,
              gpu::Timeline& readbackTL,
              gpu::Compositor::Feed feed = gpu::Compositor::Feed::Program);
    ~NdiOutput();

    int64_t framesSent() const { return sent_.load(std::memory_order_relaxed); }
    int64_t audioChunksSent() const {
        return audioSent_.load(std::memory_order_relaxed);
    }

    // Mixer thread: embed one master-bus chunk (interleaved stereo, 48 kHz).
    void sendAudio(const float* lr, int frames, int64_t firstSample);

private:
    void run(std::stop_token st);

    std::string name_;
    gpu::Compositor& comp_;
    gpu::Timeline& readbackTL_;
    gpu::Compositor::Feed feed_;
    NDIlib_send_instance_t sender_ = nullptr;
    std::atomic<int64_t> sent_{0};
    std::atomic<int64_t> audioSent_{0};
    std::vector<float> audioScratch_;  // mixer-thread only (deinterleave)
    std::jthread thread_;
};

}  // namespace moo
