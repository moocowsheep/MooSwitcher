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
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Spsc.h"
#include "engine/IInputSource.h"
#include "gpu/UploadRing.h"

typedef long long omt_receive_t;  // matches libomt.h; keep it out of headers

namespace moo {

// One OMT (Open Media Transport) input: capture thread doing omt_receive
// (UYVY preferred format -- libomt decodes VMX on the fly), one memcpy into
// pinned staging, upload submit on the transfer queue, and a latest-frame
// mailbox publish. `ref` is either a discovery name ("HOST (Name)") or a
// direct URL ("omt://host:port"); the library reconnects internally, and a
// 3 s video gap recreates the receiver as a belt-and-braces fallback.
class OmtInput : public IInputSource {
public:
    // syncFrames >= 0 enables the frame-sync feed (and sizes the upload
    // ring for that many queued frames); -1 = plain latest-frame input.
    OmtInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, std::string ref,
             int index, int syncFrames = -1);
    ~OmtInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }

    Status status() const override;
    const std::string& ref() const { return ref_; }

    void setTally(bool onProgram, bool onPreview) override {
        tally_.store(uint8_t(onProgram | (onPreview << 1)),
                     std::memory_order_relaxed);
    }

    void attachAudioSink(audio::InputChannel* ch) override {
        audioSink_.store(ch, std::memory_order_release);
    }

    SyncFeed* syncFeed() override { return syncFrames_ >= 0 ? &feed_ : nullptr; }

private:
    void run(std::stop_token st);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    std::string ref_;
    int index_;

    omt_receive_t* recv_ = nullptr;  // capture-thread owned after ctor
    std::shared_ptr<gpu::UploadRing> ring_;
    // Sticky-UYVA ring: slots whose staging alpha is already 0xFF (capture
    // thread only; sized/reset on ring rebuild).
    std::vector<uint8_t> slotAlphaOpaque_;
    Mailbox mailbox_;
    const int syncFrames_;
    SyncFeed feed_{16};
    std::atomic<audio::InputChannel*> audioSink_{nullptr};

    std::atomic<bool> connected_{false};
    std::atomic<int64_t> frames_{0};
    std::atomic<int64_t> drops_{0};
    std::atomic<uint8_t> tally_{0};  // bit0 = program, bit1 = preview
    uint8_t appliedTally_ = 0xFF;    // capture-thread local
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};

    std::jthread thread_;
};

}  // namespace moo
