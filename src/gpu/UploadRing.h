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

#include "core/Format.h"
#include "gpu/VkEngine.h"

namespace moo::gpu {

// Staging + GPU images for ONE input at ONE format: 3 pinned write-combined
// staging buffers and 3 sampled images (UYVY reinterpreted as RGBA8 at
// width/2), with an upload timeline. The capture thread owns acquire/fill/
// submit; the render side reads slots whose upload value has completed and
// pins them via renderRefs while in flight.
//
// On format change the receiver builds a new ring; the old one stays alive
// through shared_ptr refs in published frames and is destroyed on the last
// release (the destructor waits out its own timeline).
class UploadRing {
public:
    // 5 slots: at 8K an upload occupies its slot for several ms of DMA,
    // render pins up to two more across frames-in-flight, and the input
    // mailbox retains the previous publish for the late-upload fallback --
    // 4 was the minimum before that retention existed (3 starved).
    // Frame-sync inputs pass a larger count: queued frames pin their slots
    // until presented (docs/design-framesync.md).
    static constexpr int kSlots = 5;

    UploadRing(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue,
               int slots = kSlots);
    ~UploadRing();
    UploadRing(const UploadRing&) = delete;
    UploadRing& operator=(const UploadRing&) = delete;

    const VideoFormatDesc& desc() const { return desc_; }

    // Slot free for writing (upload done, no render refs), or -1.
    int acquire();
    uint8_t* stagingPtr(int slot) { return static_cast<uint8_t*>(slots_[slot].staging.mapped); }
    size_t stagingBytes() const { return desc_.frameBytes(); }

    // Records the copy + submits on the transfer queue; returns timeline value.
    uint64_t submit(int slot);

    Timeline& timeline() { return tl_; }
    const Timeline& timeline() const { return tl_; }
    VkImageView view(int slot) const { return slots_[slot].image.view; }
    VkImage image(int slot) const { return slots_[slot].image.img; }
    // Alpha plane view (UYVA rings only; VK_NULL_HANDLE otherwise).
    VkImageView viewA(int slot) const { return slots_[slot].alpha.view; }
    bool hasAlpha() const { return desc_.hasAlpha(); }

    void addRenderRef(int slot) { slots_[slot].renderRefs.fetch_add(1); }
    void releaseRenderRef(int slot) { slots_[slot].renderRefs.fetch_sub(1); }

private:
    struct Slot {
        Buffer staging;
        Image image;
        Image alpha;  // R8 full-res, UYVA rings only
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t lastSubmit = 0;
        bool imageInitialized = false;
        std::atomic<int> renderRefs{0};
    };

    VkEngine& eng_;
    VideoFormatDesc desc_;
    Queue& queue_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    Timeline tl_;
    std::unique_ptr<Slot[]> slots_;  // atomics: sized once, never moved
    int nSlots_;
    int cursor_ = 0;
};

class Nv12Ring;

// A published frame: pins one ring slot (UYVY or NV12) for render use.
struct GpuFrame {
    VideoFormatDesc desc;
    int slot = -1;
    uint64_t uploadValue = 0;
    // Per-frame, not per-format: OMT signals premultiplied alpha frame by
    // frame, and it must not force a ring rebuild.
    bool premult = false;
    std::shared_ptr<UploadRing> uyvy;  // exactly one of these is set
    std::shared_ptr<Nv12Ring> nv12;

    GpuFrame(std::shared_ptr<UploadRing> r, int s, uint64_t v,
             bool premultiplied = false);
    GpuFrame(std::shared_ptr<Nv12Ring> r, int s, uint64_t v);
    ~GpuFrame();
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;

    bool isNv12() const { return nv12 != nullptr; }
    VkImageView view() const;    // packed UYVY, or the NV12 Y plane
    VkImageView viewUV() const;  // NV12 CbCr plane; VK_NULL_HANDLE otherwise
    VkImageView viewA() const;   // UYVA alpha plane; VK_NULL_HANDLE otherwise
    const Timeline& timeline() const;
    bool uploaded() const { return timeline().completed() >= uploadValue; }
};

}  // namespace moo::gpu
