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
