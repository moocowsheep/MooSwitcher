#include "gpu/UploadRing.h"

#include "core/Log.h"
#include "gpu/Nv12Ring.h"

namespace moo::gpu {

GpuFrame::GpuFrame(std::shared_ptr<UploadRing> r, int s, uint64_t v)
    : desc(r->desc()), slot(s), uploadValue(v), uyvy(std::move(r)) {
    uyvy->addRenderRef(slot);
}

GpuFrame::GpuFrame(std::shared_ptr<Nv12Ring> r, int s, uint64_t v)
    : desc(r->desc()), slot(s), uploadValue(v), nv12(std::move(r)) {
    nv12->addRenderRef(slot);
}

GpuFrame::~GpuFrame() {
    if (uyvy) uyvy->releaseRenderRef(slot);
    if (nv12) nv12->releaseRenderRef(slot);
}

VkImageView GpuFrame::view() const {
    return nv12 ? nv12->viewY(slot) : uyvy->view(slot);
}

VkImageView GpuFrame::viewUV() const {
    return nv12 ? nv12->viewUV(slot) : VK_NULL_HANDLE;
}

const Timeline& GpuFrame::timeline() const {
    return nv12 ? nv12->timeline() : uyvy->timeline();
}

UploadRing::UploadRing(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue)
    : eng_(eng), desc_(desc), queue_(xferQueue) {
    pool_ = eng_.createCommandPool(queue_.family);
    tl_ = eng_.createTimeline();

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    for (auto& s : slots_) {
        // Upload staging: WC (coherent, deliberately not cached) -- CPU streams in.
        s.staging = eng_.createBuffer(
            desc_.frameBytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        // UYVY macropixels: one RGBA8 texel per 2 luma pixels.
        s.image = eng_.createImage2D(
            uint32_t(desc_.width / 2), uint32_t(desc_.height), VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vkAllocateCommandBuffers(eng_.device(), &cai, &s.cmd);
    }
}

UploadRing::~UploadRing() {
    // Wait out our own submissions; render refs are gone by construction
    // (frames hold the shared_ptr that keeps us alive).
    if (tl_.lastReserved()) tl_.waitCompleted(tl_.lastReserved(), 2'000'000'000);
    for (auto& s : slots_) {
        eng_.destroyBuffer(s.staging);
        eng_.destroyImage(s.image);
    }
    if (pool_) vkDestroyCommandPool(eng_.device(), pool_, nullptr);
    eng_.destroyTimeline(tl_);
}

int UploadRing::acquire() {
    for (int i = 0; i < kSlots; ++i) {
        const int s = (cursor_ + i) % kSlots;
        auto& slot = slots_[s];
        if (slot.renderRefs.load(std::memory_order_acquire) > 0) continue;
        if (slot.lastSubmit > tl_.completed()) continue;  // upload in flight
        cursor_ = (s + 1) % kSlots;
        return s;
    }
    return -1;
}

uint64_t UploadRing::submit(int slot) {
    auto& s = slots_[slot];
    vkResetCommandBuffer(s.cmd, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);

    // Everything lives in GENERAL; transition once on first use.
    if (!s.imageInitialized) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = s.image.img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(s.cmd, &di);
        s.imageInitialized = true;
    }

    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(desc_.width / 2), uint32_t(desc_.height), 1};
    VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    ci.srcBuffer = s.staging.buf;
    ci.dstImage = s.image.img;
    ci.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBufferToImage2(s.cmd, &ci);
    vkEndCommandBuffer(s.cmd);

    const uint64_t value = tl_.reserve();
    const VkSemaphoreSubmitInfo signal =
        VkEngine::timelineSignal(tl_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
    VkEngine::SubmitDesc sd;
    sd.cmd = s.cmd;
    sd.signalInfos = {&signal, 1};
    if (eng_.submit(queue_, sd) != VK_SUCCESS) MOO_LOGE("upload submit failed");
    s.lastSubmit = value;
    return value;
}

}  // namespace moo::gpu
