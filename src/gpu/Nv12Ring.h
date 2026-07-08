#pragma once
#include <atomic>
#include <memory>

#include "core/Format.h"
#include "gpu/VkEngine.h"

namespace moo::gpu {

// GPU-side ingest ring for NV12 sources (SRT/NVDEC): per slot, an EXPORTABLE
// device-local staging buffer (CUDA writes decoded planes into it through
// external-memory import — the media layer owns that import) plus Y (R8) and
// CbCr (R8G8, half-res) sampled images filled by buffer->image copies on a
// transfer queue. Same acquire/renderRef discipline as UploadRing.
class Nv12Ring {
public:
    // One extra over the pre-M5 four: the input mailbox retains the previous
    // publish for the late-upload fallback (see LatestMailbox). Frame-sync
    // inputs pass a larger count: queued frames pin their slots until
    // presented (docs/design-framesync.md).
    static constexpr int kSlots = 5;

    Nv12Ring(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue,
             int slots = kSlots);
    ~Nv12Ring();
    Nv12Ring(const Nv12Ring&) = delete;
    Nv12Ring& operator=(const Nv12Ring&) = delete;

    const VideoFormatDesc& desc() const { return desc_; }
    size_t stagingBytes() const { return desc_.frameBytes(); }  // w*h*3/2

    int acquire();
    // New fd for the slot's staging memory; the importer consumes it.
    int exportStagingFd(int slot) { return eng_.exportMemoryFd(slots_[slot].staging); }

    // Records Y+UV buffer->image copies, submits, returns timeline value.
    // Caller must have finished (synchronized) its CUDA writes first.
    uint64_t submit(int slot);

    Timeline& timeline() { return tl_; }
    const Timeline& timeline() const { return tl_; }
    VkImageView viewY(int slot) const { return slots_[slot].y.view; }
    VkImageView viewUV(int slot) const { return slots_[slot].uv.view; }

    void addRenderRef(int slot) { slots_[slot].renderRefs.fetch_add(1); }
    void releaseRenderRef(int slot) { slots_[slot].renderRefs.fetch_sub(1); }
    int slotCount() const { return nSlots_; }

private:
    struct Slot {
        Buffer staging;
        Image y, uv;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        uint64_t lastSubmit = 0;
        bool imagesInitialized = false;
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

}  // namespace moo::gpu
