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
    static constexpr int kSlots = 3;

    UploadRing(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue);
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

    void addRenderRef(int slot) { slots_[slot].renderRefs.fetch_add(1); }
    void releaseRenderRef(int slot) { slots_[slot].renderRefs.fetch_sub(1); }

private:
    struct Slot {
        Buffer staging;
        Image image;
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
    Slot slots_[kSlots];
    int cursor_ = 0;
};

// A published frame: pins one ring slot for render use.
struct GpuFrame {
    std::shared_ptr<UploadRing> ring;
    int slot = -1;
    uint64_t uploadValue = 0;
    VideoFormatDesc desc;

    GpuFrame(std::shared_ptr<UploadRing> r, int s, uint64_t v)
        : ring(std::move(r)), slot(s), uploadValue(v), desc(ring->desc()) {
        ring->addRenderRef(slot);
    }
    ~GpuFrame() { ring->releaseRenderRef(slot); }
    GpuFrame(const GpuFrame&) = delete;
    GpuFrame& operator=(const GpuFrame&) = delete;

    bool uploaded() const { return ring->timeline().completed() >= uploadValue; }
};

}  // namespace moo::gpu
