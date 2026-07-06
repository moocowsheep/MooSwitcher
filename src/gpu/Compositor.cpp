#include "gpu/Compositor.h"

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "shaders/composite_comp.spv.h"
#include "shaders/multiview_tile_comp.spv.h"

namespace moo::gpu {

// C++ mirrors of the GLSL push-constant blocks (std430 offsets).
struct Compositor::CompositePC {
    int32_t outW, outH, aW, aH, bW, bH, padX, padY;
    float aMap[4], bMap[4];
    float alpha, softness, ftb;
    int32_t transType, aCm, bCm;
};

struct Compositor::TilePC {
    int32_t ox, oy, dw, dh, sw, sh, padX, padY;
    float map[4];
    int32_t mode, cm;
};

namespace {

VkShaderModule makeModule(VkDevice dev, const uint8_t* code, size_t size) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = reinterpret_cast<const uint32_t*>(code);
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

// Aspect-fit UV mapping: uvIn = uv * map.xy + map.zw; outside [0,1] = black.
void fitMap(int srcW, int srcH, int dstW, int dstH, float out[4]) {
    const double k = std::min(double(dstW) / srcW, double(dstH) / srcH);
    const double fx = srcW * k / dstW, fy = srcH * k / dstH;
    out[0] = float(1.0 / fx);
    out[1] = float(1.0 / fy);
    out[2] = float(0.5 - 0.5 / fx);
    out[3] = float(0.5 - 0.5 / fy);
}

}  // namespace

Compositor::Compositor(VkEngine& eng, const VideoFormatDesc& show, int mvW, int mvH)
    : eng_(eng), show_(show), mvW_(mvW & ~1), mvH_(mvH & ~1) {
    for (int f = 0; f < kFramesInFlight; ++f) {
        program_[f] = eng_.createImage2D(
            uint32_t(show_.width), uint32_t(show_.height), VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        multiview_[f] = eng_.createImage2D(
            uint32_t(mvW_), uint32_t(mvH_), VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    for (auto& rb : readback_)
        rb = eng_.createBuffer(readbackBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    createPipelines();
}

Compositor::~Compositor() {
    VkDevice dev = eng_.device();
    if (compositePipe_) vkDestroyPipeline(dev, compositePipe_, nullptr);
    if (tilePipe_) vkDestroyPipeline(dev, tilePipe_, nullptr);
    if (compositeLayout_) vkDestroyPipelineLayout(dev, compositeLayout_, nullptr);
    if (tileLayout_) vkDestroyPipelineLayout(dev, tileLayout_, nullptr);
    if (compositeDsl_) vkDestroyDescriptorSetLayout(dev, compositeDsl_, nullptr);
    if (tileDsl_) vkDestroyDescriptorSetLayout(dev, tileDsl_, nullptr);
    for (auto& i : program_) eng_.destroyImage(i);
    for (auto& i : multiview_) eng_.destroyImage(i);
    for (auto& b : readback_) eng_.destroyBuffer(b);
}

void Compositor::createPipelines() {
    // GLSL std430 push-constant mirrors must match byte-for-byte.
    static_assert(sizeof(CompositePC) == 88);
    static_assert(sizeof(TilePC) == 56);
    VkDevice dev = eng_.device();

    auto makeDsl = [&](std::initializer_list<VkDescriptorType> types) {
        std::vector<VkDescriptorSetLayoutBinding> binds;
        uint32_t i = 0;
        for (auto t : types)
            binds.push_back({i++, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ci.bindingCount = uint32_t(binds.size());
        ci.pBindings = binds.data();
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &ci, nullptr, &dsl);
        return dsl;
    };
    compositeDsl_ = makeDsl({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
    tileDsl_ = makeDsl({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});

    auto makeLayout = [&](VkDescriptorSetLayout dsl, uint32_t pcSize) {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = &dsl;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pcr;
        VkPipelineLayout l = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &ci, nullptr, &l);
        return l;
    };
    compositeLayout_ = makeLayout(compositeDsl_, sizeof(CompositePC));
    tileLayout_ = makeLayout(tileDsl_, sizeof(TilePC));

    auto makePipe = [&](const uint8_t* spv, size_t size, VkPipelineLayout layout) {
        VkShaderModule mod = makeModule(dev, spv, size);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = layout;
        VkPipeline p = VK_NULL_HANDLE;
        vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p);
        vkDestroyShaderModule(dev, mod, nullptr);
        return p;
    };
    compositePipe_ = makePipe(shaders::composite_comp, shaders::composite_comp_size,
                              compositeLayout_);
    tilePipe_ =
        makePipe(shaders::multiview_tile_comp, shaders::multiview_tile_comp_size,
                 tileLayout_);
    if (!compositePipe_ || !tilePipe_) MOO_LOGE("compute pipeline creation failed");
}

void Compositor::barrier(VkCommandBuffer cmd, VkImage img,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                         VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void Compositor::dispatchTile(VkCommandBuffer cmd, VkImageView src, bool srcIsRgb,
                              const VideoFormatDesc* srcDesc, int dstX, int dstY,
                              int dstW, int dstH, int fif) {
    VkDescriptorImageInfo srcInfo{eng_.linearSampler(), src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, multiview_[fif].view,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE,
                 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo,
                 nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE,
                 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstInfo, nullptr,
                 nullptr};
    eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tileLayout_, 0, 2,
                              writes);

    TilePC pc{};
    pc.ox = dstX;
    pc.oy = dstY;
    pc.dw = dstW;
    pc.dh = dstH;
    if (srcIsRgb) {
        pc.sw = show_.width;
        pc.sh = show_.height;
        fitMap(show_.width, show_.height, dstW, dstH, pc.map);
        pc.mode = 1;
        pc.cm = 0;
    } else {
        pc.sw = srcDesc->width;
        pc.sh = srcDesc->height;
        fitMap(srcDesc->width, srcDesc->height, dstW, dstH, pc.map);
        pc.mode = 0;
        pc.cm = srcDesc->colorimetry == Colorimetry::BT601 ? 1 : 0;
    }
    vkCmdPushConstants(cmd, tileLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                       &pc);
    vkCmdDispatch(cmd, uint32_t((dstW + 15) / 16), uint32_t((dstH + 15) / 16), 1);
}

void Compositor::record(VkCommandBuffer cmd, const TickJob& job, int fif,
                        int rbSlot) {
    auto& prog = program_[fif];
    auto& mv = multiview_[fif];

    if (!targetsInitialized_[fif]) {
        for (VkImage img : {prog.img, mv.img})
            barrier(cmd, img, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        targetsInitialized_[fif] = true;
    }

    // -- composite: a/b UYVY -> program RGBA16F --
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compositePipe_);
    VkDescriptorImageInfo aInfo{eng_.linearSampler(), job.a->ring->view(job.a->slot),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo bInfo{eng_.linearSampler(), job.b->ring->view(job.b->slot),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo pInfo{VK_NULL_HANDLE, prog.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aInfo, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bInfo, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &pInfo, nullptr, nullptr};
    eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compositeLayout_,
                              0, 3, w);

    CompositePC cpc{};
    cpc.outW = show_.width;
    cpc.outH = show_.height;
    cpc.aW = job.a->desc.width;
    cpc.aH = job.a->desc.height;
    cpc.bW = job.b->desc.width;
    cpc.bH = job.b->desc.height;
    fitMap(cpc.aW, cpc.aH, cpc.outW, cpc.outH, cpc.aMap);
    fitMap(cpc.bW, cpc.bH, cpc.outW, cpc.outH, cpc.bMap);
    cpc.alpha = job.sw.alpha;
    cpc.softness = job.sw.softness;
    cpc.ftb = job.sw.ftb;
    cpc.transType = int32_t(job.sw.trans);
    cpc.aCm = job.a->desc.colorimetry == Colorimetry::BT601 ? 1 : 0;
    cpc.bCm = job.b->desc.colorimetry == Colorimetry::BT601 ? 1 : 0;
    vkCmdPushConstants(cmd, compositeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, uint32_t((show_.width + 15) / 16),
                  uint32_t((show_.height + 15) / 16), 1);

    // program written (compute) -> sampled by tiles (compute)
    barrier(cmd, prog.img, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);

    // -- multiview: clear, tiles, readback --
    barrier(cmd, mv.img, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, mv.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    barrier(cmd, mv.img, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tilePipe_);
    const int topH = mvH_ * 2 / 3 & ~1;
    const int halfW = mvW_ / 2 & ~1;
    dispatchTile(cmd, prog.view, true, nullptr, 0, 0, halfW, topH, fif);  // PGM
    if (job.preview)
        dispatchTile(cmd, job.preview->ring->view(job.preview->slot), false,
                     &job.preview->desc, halfW, 0, mvW_ - halfW, topH, fif);  // PVW
    const int rowY = topH;
    const int rowH = mvH_ - topH;
    const int cells = std::max<int>(4, int(job.mvInputs.size()));
    const int cellW = (mvW_ / cells) & ~1;
    for (size_t i = 0; i < job.mvInputs.size(); ++i) {
        const auto* f = job.mvInputs[i].frame;
        if (!f) continue;
        dispatchTile(cmd, f->ring->view(f->slot), false, &f->desc, int(i) * cellW,
                     rowY, cellW, rowH, fif);
    }

    // tiles written (compute) -> copy out (transfer)
    barrier(cmd, mv.img, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(mvW_), uint32_t(mvH_), 1};
    VkCopyImageToBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    ci.srcImage = mv.img;
    ci.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.dstBuffer = readback_[rbSlot].buf;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyImageToBuffer2(cmd, &ci);
}

}  // namespace moo::gpu
