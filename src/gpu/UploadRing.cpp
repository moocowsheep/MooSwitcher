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

#include "gpu/UploadRing.h"

#include "core/Log.h"
#include "gpu/Nv12Ring.h"

namespace moo::gpu {

GpuFrame::GpuFrame(std::shared_ptr<UploadRing> r, int s, uint64_t v,
                   bool premultiplied)
    : desc(r->desc()), slot(s), uploadValue(v), premult(premultiplied),
      uyvy(std::move(r)) {
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

VkImageView GpuFrame::viewA() const {
    return uyvy ? uyvy->viewA(slot) : VK_NULL_HANDLE;
}

const Timeline& GpuFrame::timeline() const {
    return nv12 ? nv12->timeline() : uyvy->timeline();
}

UploadRing::UploadRing(VkEngine& eng, const VideoFormatDesc& desc, Queue& xferQueue,
                       int slots)
    : eng_(eng), desc_(desc), queue_(xferQueue),
      slots_(new Slot[size_t(slots)]), nSlots_(slots) {
    pool_ = eng_.createCommandPool(queue_.family);
    tl_ = eng_.createTimeline();

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    for (int i = 0; i < nSlots_; ++i) {
        auto& s = slots_[i];
        // Upload staging: WC (coherent, deliberately not cached) -- CPU streams in.
        s.staging = eng_.createBuffer(
            desc_.frameBytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        // UYVY macropixels: one RGBA8 texel per 2 luma pixels.
        s.image = eng_.createImage2D(
            uint32_t(desc_.width / 2), uint32_t(desc_.height), VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (desc_.hasAlpha())
            s.alpha = eng_.createImage2D(
                uint32_t(desc_.width), uint32_t(desc_.height), VK_FORMAT_R8_UNORM,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        vkAllocateCommandBuffers(eng_.device(), &cai, &s.cmd);
    }
}

UploadRing::~UploadRing() {
    // Wait out our own submissions; render refs are gone by construction
    // (frames hold the shared_ptr that keeps us alive).
    if (tl_.lastReserved()) tl_.waitCompleted(tl_.lastReserved(), 2'000'000'000);
    for (int i = 0; i < nSlots_; ++i) {
        eng_.destroyBuffer(slots_[i].staging);
        eng_.destroyImage(slots_[i].image);
        eng_.destroyImage(slots_[i].alpha);  // null-safe when absent
    }
    if (pool_) vkDestroyCommandPool(eng_.device(), pool_, nullptr);
    eng_.destroyTimeline(tl_);
}

int UploadRing::acquire() {
    for (int i = 0; i < nSlots_; ++i) {
        const int s = (cursor_ + i) % nSlots_;
        auto& slot = slots_[s];
        if (slot.renderRefs.load(std::memory_order_acquire) > 0) continue;
        if (slot.lastSubmit > tl_.completed()) continue;  // upload in flight
        cursor_ = (s + 1) % nSlots_;
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
        VkImageMemoryBarrier2 b[2] = {};
        uint32_t nb = s.alpha.img ? 2u : 1u;
        for (uint32_t i = 0; i < nb; ++i) {
            b[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b[i].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        b[0].image = s.image.img;
        if (s.alpha.img) b[1].image = s.alpha.img;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = nb;
        di.pImageMemoryBarriers = b;
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

    if (s.alpha.img) {
        // Appended full-res alpha plane (see VideoFormatDesc::alphaOffset).
        VkBufferImageCopy2 ar{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        ar.bufferOffset = desc_.alphaOffset();
        ar.bufferRowLength = uint32_t(desc_.width);  // texels of R8
        ar.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        ar.imageExtent = {uint32_t(desc_.width), uint32_t(desc_.height), 1};
        VkCopyBufferToImageInfo2 ac{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ac.srcBuffer = s.staging.buf;
        ac.dstImage = s.alpha.img;
        ac.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ac.regionCount = 1;
        ac.pRegions = &ar;
        vkCmdCopyBufferToImage2(s.cmd, &ac);
    }
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
