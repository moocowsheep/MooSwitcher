/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "gpu/Nv12Ring.h"

#include "core/Log.h"
#include "gpu/UploadRing.h"

namespace moo::gpu {

Nv12Ring::Nv12Ring(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue,
                   int slots)
    : eng_(eng), desc_(desc), queue_(xferQueue),
      slots_(new Slot[size_t(slots)]), nSlots_(slots) {
    desc_.pixfmt = PixFmt::NV12;
    pool_ = eng_.createCommandPool(queue_.family);
    tl_ = eng_.createTimeline();

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    for (int i = 0; i < nSlots_; ++i) {
        auto& s = slots_[i];
        s.staging = eng_.createBuffer(desc_.frameBytes(),
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0,
                                      /*exportable=*/true);
        s.y = eng_.createImage2D(uint32_t(desc_.width), uint32_t(desc_.height),
                                 VK_FORMAT_R8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        s.uv = eng_.createImage2D(uint32_t(desc_.width / 2),
                                  uint32_t(desc_.height / 2), VK_FORMAT_R8G8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vkAllocateCommandBuffers(eng_.device(), &cai, &s.cmd);
    }
}

Nv12Ring::~Nv12Ring() {
    if (tl_.lastReserved()) tl_.waitCompleted(tl_.lastReserved(), 2'000'000'000);
    for (int i = 0; i < nSlots_; ++i) {
        eng_.destroyBuffer(slots_[i].staging);
        eng_.destroyImage(slots_[i].y);
        eng_.destroyImage(slots_[i].uv);
    }
    if (pool_) vkDestroyCommandPool(eng_.device(), pool_, nullptr);
    eng_.destroyTimeline(tl_);
}

int Nv12Ring::acquire() {
    for (int i = 0; i < nSlots_; ++i) {
        const int s = (cursor_ + i) % nSlots_;
        auto& slot = slots_[s];
        if (slot.renderRefs.load(std::memory_order_acquire) > 0) continue;
        if (slot.lastSubmit > tl_.completed()) continue;
        cursor_ = (s + 1) % nSlots_;
        return s;
    }
    return -1;
}

uint64_t Nv12Ring::submit(int slot) {
    auto& s = slots_[slot];
    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cmd, &bi);

    if (!s.imagesInitialized) {
        VkImageMemoryBarrier2 b[2] = {};
        for (int i = 0; i < 2; ++i) {
            b[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        b[0].image = s.y.img;
        b[1].image = s.uv.img;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 2;
        di.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(s.cmd, &di);
        s.imagesInitialized = true;
    }

    // NV12 layout in the staging buffer: Y plane (w*h), then CbCr (w*h/2).
    VkBufferImageCopy2 yr{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    yr.bufferOffset = 0;
    yr.bufferRowLength = uint32_t(desc_.width);
    yr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    yr.imageExtent = {uint32_t(desc_.width), uint32_t(desc_.height), 1};
    VkCopyBufferToImageInfo2 yc{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    yc.srcBuffer = s.staging.buf;
    yc.dstImage = s.y.img;
    yc.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    yc.regionCount = 1;
    yc.pRegions = &yr;
    vkCmdCopyBufferToImage2(s.cmd, &yc);

    VkBufferImageCopy2 cr{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    cr.bufferOffset = VkDeviceSize(desc_.width) * desc_.height;
    cr.bufferRowLength = uint32_t(desc_.width / 2);  // texels of R8G8
    cr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cr.imageExtent = {uint32_t(desc_.width / 2), uint32_t(desc_.height / 2), 1};
    VkCopyBufferToImageInfo2 cc{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    cc.srcBuffer = s.staging.buf;
    cc.dstImage = s.uv.img;
    cc.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    cc.regionCount = 1;
    cc.pRegions = &cr;
    vkCmdCopyBufferToImage2(s.cmd, &cc);
    vkEndCommandBuffer(s.cmd);

    const uint64_t value = tl_.reserve();
    const VkSemaphoreSubmitInfo signal =
        VkEngine::timelineSignal(tl_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
    VkEngine::SubmitDesc sd;
    sd.cmd = s.cmd;
    sd.signalInfos = {&signal, 1};
    if (eng_.submit(queue_, sd) != VK_SUCCESS) MOO_LOGE("nv12 upload submit failed");
    s.lastSubmit = value;
    return value;
}

}  // namespace moo::gpu
