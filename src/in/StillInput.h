/* Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "engine/IInputSource.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"

struct AVFrame;

namespace moo {

// A local raster image decoded once on a worker thread and retained as a
// static switcher input. RGB is converted to the engine's limited-range UYVY
// contract; non-opaque source alpha is retained as a straight full-resolution
// plane so PNG/WebP graphics can be used directly by a downstream keyer.
class StillInput final : public IInputSource {
public:
    StillInput(gpu::VkEngine& eng, gpu::Queue& uploadQueue, std::string path,
               int index, int64_t fpsN, int64_t fpsD);
    ~StillInput() override;

    std::optional<Mailbox::Item> newer(uint64_t lastSeq) const override {
        return mailbox_.takeNewer(lastSeq);
    }
    int newerCandidates(uint64_t lastSeq, Mailbox::Item* out) const override {
        return mailbox_.takeNewerCandidates(lastSeq, out);
    }
    Status status() const override;
    bool frameIsPersistent() const override { return true; }

private:
    void run(std::stop_token stop);
    bool publishFrame(const AVFrame* frame);
    static int interruptCb(void* opaque);

    gpu::VkEngine& eng_;
    gpu::Queue& queue_;
    std::string path_;
    int index_;
    int64_t fpsN_;
    int64_t fpsD_;

    std::shared_ptr<gpu::UploadRing> ring_;
    Mailbox mailbox_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<int64_t> frames_{0};
    mutable std::mutex descM_;
    VideoFormatDesc desc_{};
    std::jthread thread_;
};

}  // namespace moo
