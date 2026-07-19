#include "gpu/Compositor.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "core/Log.h"
#include "shaders/composite_comp.spv.h"
#include "shaders/multiview_tile_comp.spv.h"
#include "shaders/pack_nv12_comp.spv.h"
#include "shaders/pack_uyvy_comp.spv.h"
#include "shaders/proxy_down_comp.spv.h"

namespace moo::gpu {

// C++ mirrors of the GLSL push-constant blocks (std430 offsets).
struct Compositor::CompositePC {
    int32_t outW, outH, aW, aH, bW, bH, aFmt, bFmt;
    float aMap[4], bMap[4];
    float alpha, softness, ftb;
    int32_t transType, aCm, bCm;
    // Downstream keyers. flags: bit0 NV12, bit1 alpha plane, bit2
    // premultiplied, bit3 BT.601. vec4 maps sit at offsets 96/128
    // (16-aligned, matching the GLSL std430 push-constant block).
    int32_t k1Flags;
    float k1Level;
    float k1Map[4];
    int32_t k1FullW, k1FullH;
    int32_t k2Flags;
    float k2Level;
    float k2Map[4];
    int32_t k2FullW, k2FullH;
    int32_t cleanEnabled;
};
struct Compositor::TilePC {
    int32_t ox, oy, dw, dh, sw, sh, padX, padY;
    float map[4];
    int32_t mode, cm;
};
struct Compositor::PackPC {
    int32_t w, h, wordsPerRow, cm;
};
struct Compositor::ProxyPC {
    int32_t dw, dh, sw, sh, mode, cm;
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

// Aspect-fit UV mapping with an optional used-fraction (proxy sub-extents).
void fitMap(int srcW, int srcH, int dstW, int dstH, float out[4], float ufX = 1.f,
            float ufY = 1.f) {
    const double k = std::min(double(dstW) / srcW, double(dstH) / srcH);
    const double fx = srcW * k / dstW, fy = srcH * k / dstH;
    const double mx = 1.0 / fx, my = 1.0 / fy;
    const double zx = 0.5 - 0.5 / fx, zy = 0.5 - 0.5 / fy;
    out[0] = float(mx * ufX);
    out[1] = float(my * ufY);
    out[2] = float(zx * ufX);
    out[3] = float(zy * ufY);
}

constexpr float kTallyRed[3] = {0.85f, 0.10f, 0.10f};
constexpr float kTallyGreen[3] = {0.10f, 0.70f, 0.12f};
constexpr int kBorder = 3;

}  // namespace

Compositor::Compositor(VkEngine& eng, const VideoFormatDesc& show, int mvW,
                       int mvH, int numInputs)
    : eng_(eng), show_(show), mvW_(mvW & ~1), mvH_(mvH & ~1), numInputs_(numInputs) {
    static_assert(sizeof(CompositePC) == 156);
    static_assert(sizeof(TilePC) == 56);
    // 152 exceeds the 128-byte Vulkan minimum; NVIDIA guarantees 256. Fail
    // loudly on anything smaller rather than truncating the block.
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(eng_.physical(), &props);
        if (props.limits.maxPushConstantsSize < sizeof(CompositePC)) {
            MOO_LOGE("maxPushConstantsSize %u < CompositePC %zu -- unsupported "
                     "device",
                     props.limits.maxPushConstantsSize, sizeof(CompositePC));
            std::abort();
        }
    }
    static_assert(sizeof(PackPC) == 16);
    static_assert(sizeof(ProxyPC) == 24);

    for (int f = 0; f < kFramesInFlight; ++f) {
        program_[f] = eng_.createImage2D(
            uint32_t(show_.width), uint32_t(show_.height),
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        clean_[f] = eng_.createImage2D(
            uint32_t(show_.width), uint32_t(show_.height),
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        multiview_[f] = eng_.createImage2D(
            uint32_t(mvW_), uint32_t(mvH_), VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        programProxy_[f] = eng_.createImage2D(
            kProxyW, kProxyH, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        inputProxy_[f].resize(size_t(numInputs_));
        for (auto& p : inputProxy_[f])
            p = eng_.createImage2D(kProxyW, kProxyH, VK_FORMAT_R8G8B8A8_UNORM,
                                   VK_IMAGE_USAGE_STORAGE_BIT |
                                       VK_IMAGE_USAGE_SAMPLED_BIT);
        proxyInit_[f].assign(size_t(numInputs_) + 1, false);

        for (int feed = 0; feed < kFeedCount; ++feed) {
            packDev_[feed][f] = eng_.createBuffer(
                show_.frameBytes(),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            packNvDev_[feed][f] = eng_.createBuffer(
                nvPackBytes(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0,
                /*exportable=*/eng_.hasExternalMemoryFd);
        }
    }
    for (auto& rb : readback_)
        rb = eng_.createBuffer(readbackBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                               VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    for (auto& feed : packHost_)
        for (auto& pb : feed)
            pb = eng_.createBuffer(
                show_.frameBytes(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                VK_MEMORY_PROPERTY_HOST_CACHED_BIT);

    composite_ = makePipe(shaders::composite_comp, shaders::composite_comp_size,
                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                           VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                          sizeof(CompositePC));
    tile_ = makePipe(shaders::multiview_tile_comp, shaders::multiview_tile_comp_size,
                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                     sizeof(TilePC));
    pack_ = makePipe(shaders::pack_uyvy_comp, shaders::pack_uyvy_comp_size,
                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                     sizeof(PackPC));
    packNv_ = makePipe(shaders::pack_nv12_comp, shaders::pack_nv12_comp_size,
                       {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                       sizeof(PackPC));
    proxy_ = makePipe(shaders::proxy_down_comp, shaders::proxy_down_comp_size,
                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                       VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                      sizeof(ProxyPC));
}

Compositor::~Compositor() {
    destroyPipe(composite_);
    destroyPipe(tile_);
    destroyPipe(pack_);
    destroyPipe(packNv_);
    destroyPipe(proxy_);
    for (int f = 0; f < kFramesInFlight; ++f) {
        eng_.destroyImage(program_[f]);
        eng_.destroyImage(clean_[f]);
        eng_.destroyImage(multiview_[f]);
        eng_.destroyImage(programProxy_[f]);
        for (auto& p : inputProxy_[f]) eng_.destroyImage(p);
        for (int feed = 0; feed < kFeedCount; ++feed) {
            eng_.destroyBuffer(packDev_[feed][f]);
            eng_.destroyBuffer(packNvDev_[feed][f]);
        }
    }
    for (auto& b : readback_) eng_.destroyBuffer(b);
    for (auto& feed : packHost_)
        for (auto& b : feed) eng_.destroyBuffer(b);
    if (labelAtlas_.img) eng_.destroyImage(labelAtlas_);
}

Compositor::Pipe Compositor::makePipe(const uint8_t* spv, size_t size,
                                      std::initializer_list<VkDescriptorType> bindings,
                                      uint32_t pcSize) {
    VkDevice dev = eng_.device();
    Pipe p;

    std::vector<VkDescriptorSetLayoutBinding> binds;
    uint32_t i = 0;
    for (auto t : bindings)
        binds.push_back({i++, t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo dci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = uint32_t(binds.size());
    dci.pBindings = binds.data();
    vkCreateDescriptorSetLayout(dev, &dci, nullptr, &p.dsl);

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &p.dsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(dev, &lci, nullptr, &p.layout);

    VkShaderModule mod = makeModule(dev, spv, size);
    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    cci.layout = p.layout;
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, nullptr, &p.pipe);
    vkDestroyShaderModule(dev, mod, nullptr);
    if (!p.pipe) MOO_LOGE("compute pipeline creation failed");
    return p;
}

void Compositor::destroyPipe(Pipe& p) {
    VkDevice dev = eng_.device();
    if (p.pipe) vkDestroyPipeline(dev, p.pipe, nullptr);
    if (p.layout) vkDestroyPipelineLayout(dev, p.layout, nullptr);
    if (p.dsl) vkDestroyDescriptorSetLayout(dev, p.dsl, nullptr);
    p = {};
}

void Compositor::setLabelAtlas(Image atlas, std::vector<int> usedWidths) {
    if (labelAtlas_.img) eng_.destroyImage(labelAtlas_);
    labelAtlas_ = atlas;
    labelUsedW_ = std::move(usedWidths);
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

void Compositor::memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                            VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                            VkAccessFlags2 dstAccess) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void Compositor::initImageOnce(VkCommandBuffer cmd, Image& img, uint8_t& flag) {
    if (flag) return;
    barrier(cmd, img.img, VK_PIPELINE_STAGE_2_NONE, 0,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    flag = true;
}

void Compositor::proxyUsed(const VideoFormatDesc& d, int& w, int& h) {
    if (d.width <= kProxyW && d.height <= kProxyH) {
        w = d.width & ~1;
        h = d.height & ~1;
        return;
    }
    const double k = std::min(double(kProxyW) / d.width, double(kProxyH) / d.height);
    w = int(d.width * k) & ~1;
    h = int(d.height * k) & ~1;
    w = std::max(w, 2);
    h = std::max(h, 2);
}

void Compositor::dispatchProxy(VkCommandBuffer cmd, VkImageView src,
                               VkImageView srcUv, int mode,
                               const VideoFormatDesc* srcDesc, Image& dst,
                               int usedW, int usedH) {
    VkDescriptorImageInfo srcInfo{eng_.linearSampler(), src, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo uvInfo{eng_.linearSampler(), srcUv ? srcUv : src,
                                 VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, dst.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &uvInfo, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstInfo, nullptr, nullptr};
    eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, proxy_.layout, 0,
                              3, w);
    ProxyPC pc{};
    pc.dw = usedW;
    pc.dh = usedH;
    pc.mode = mode;
    if (srcDesc) {
        pc.sw = srcDesc->width;
        pc.sh = srcDesc->height;
        pc.cm = srcDesc->colorimetry == Colorimetry::BT601 ? 1 : 0;
    }
    vkCmdPushConstants(cmd, proxy_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                       &pc);
    vkCmdDispatch(cmd, uint32_t((usedW + 15) / 16), uint32_t((usedH + 15) / 16), 1);
}

void Compositor::dispatchTile(VkCommandBuffer cmd, VkImageView src,
                              VkImageView srcUv, int mode, const float srcMap[4],
                              const VideoFormatDesc* srcDesc, int dstX, int dstY,
                              int dstW, int dstH, int fif) {
    VkDescriptorImageInfo srcInfo{eng_.linearSampler(),
                                  src ? src : labelAtlas_.view,  // any valid view
                                  VK_IMAGE_LAYOUT_GENERAL};
    if (!srcInfo.imageView) srcInfo.imageView = multiview_[fif].view;
    VkDescriptorImageInfo uvInfo{eng_.linearSampler(),
                                 srcUv ? srcUv : srcInfo.imageView,
                                 VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo dstInfo{VK_NULL_HANDLE, multiview_[fif].view,
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &uvInfo, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &dstInfo, nullptr, nullptr};
    eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tile_.layout, 0, 3,
                              w);
    TilePC pc{};
    pc.ox = dstX;
    pc.oy = dstY;
    pc.dw = dstW;
    pc.dh = dstH;
    if (srcDesc) {
        pc.sw = srcDesc->width;
        pc.sh = srcDesc->height;
        pc.cm = srcDesc->colorimetry == Colorimetry::BT601 ? 1 : 0;
    }
    memcpy(pc.map, srcMap, sizeof(pc.map));
    pc.mode = mode;
    vkCmdPushConstants(cmd, tile_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc),
                       &pc);
    vkCmdDispatch(cmd, uint32_t((dstW + 15) / 16), uint32_t((dstH + 15) / 16), 1);
}

void Compositor::tileFromProxy(VkCommandBuffer cmd, const Image& proxy, int usedW,
                               int usedH, int dstX, int dstY, int dstW, int dstH,
                               int fif) {
    float map[4];
    fitMap(usedW, usedH, dstW, dstH, map, float(usedW) / kProxyW,
           float(usedH) / kProxyH);
    dispatchTile(cmd, proxy.view, VK_NULL_HANDLE, 1, map, nullptr, dstX, dstY, dstW,
                 dstH, fif);
}

void Compositor::labelTile(VkCommandBuffer cmd, int row, int dstX, int dstY,
                           int dstW, int fif) {
    if (!labelAtlas_.img || row >= int(labelUsedW_.size())) return;
    const int usedW = std::min(labelUsedW_[size_t(row)], dstW);
    const float aw = float(labelAtlas_.width), ah = float(labelAtlas_.height);
    float map[4] = {usedW / aw, kLabelRowH / ah, 0.f, row * kLabelRowH / ah};
    dispatchTile(cmd, labelAtlas_.view, VK_NULL_HANDLE, 1, map, nullptr, dstX, dstY,
                 usedW, kLabelRowH, fif);
}

void Compositor::borderTiles(VkCommandBuffer cmd, int x, int y, int w, int h,
                             const float rgb[3], int fif) {
    const float map[4] = {rgb[0], rgb[1], rgb[2], 0.f};
    dispatchTile(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, map, nullptr, x, y, w,
                 kBorder, fif);
    dispatchTile(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, map, nullptr, x,
                 y + h - kBorder, w, kBorder, fif);
    dispatchTile(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, map, nullptr, x, y, kBorder,
                 h, fif);
    dispatchTile(cmd, VK_NULL_HANDLE, VK_NULL_HANDLE, 2, map, nullptr,
                 x + w - kBorder, y, kBorder, h, fif);
}

void Compositor::record(VkCommandBuffer cmd, const TickJob& job, int fif,
                        int rbSlot) {
    auto& prog = program_[fif];
    auto& clean = clean_[fif];
    auto& mv = multiview_[fif];

    if (!targetsInit_[fif]) {
        for (VkImage img : {prog.img, clean.img, mv.img})
            barrier(cmd, img, VK_PIPELINE_STAGE_2_NONE, 0,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        targetsInit_[fif] = true;
    }
    initImageOnce(cmd, programProxy_[fif], proxyInit_[fif][size_t(numInputs_)]);
    for (int i = 0; i < numInputs_; ++i)
        initImageOnce(cmd, inputProxy_[fif][size_t(i)], proxyInit_[fif][size_t(i)]);

    // -- composite: a/b (UYVY or NV12) -> program RGBA16F --
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_.pipe);
    VkDescriptorImageInfo aInfo{eng_.linearSampler(), job.a->view(),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo aUvInfo{eng_.linearSampler(),
                                  job.a->viewUV() ? job.a->viewUV() : job.a->view(),
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo bInfo{eng_.linearSampler(), job.b->view(),
                                VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo bUvInfo{eng_.linearSampler(),
                                  job.b->viewUV() ? job.b->viewUV() : job.b->view(),
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo pInfo{VK_NULL_HANDLE, prog.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo cleanInfo{VK_NULL_HANDLE, clean.view,
                                    VK_IMAGE_LAYOUT_GENERAL};
    // Keyers: aux = NV12 UV | UYVA alpha | self-rebound fill (never NULL,
    // same pattern as the UV slots above). Dark keyer falls back to job.a.
    VkDescriptorImageInfo kInfo[kDskCount], kAuxInfo[kDskCount];
    for (int k = 0; k < kDskCount; ++k) {
        const GpuFrame* f = job.dsk[k] ? job.dsk[k] : job.a;
        VkImageView aux = f->viewUV() ? f->viewUV()
                          : f->viewA() ? f->viewA()
                                       : f->view();
        kInfo[k] = {eng_.linearSampler(), f->view(), VK_IMAGE_LAYOUT_GENERAL};
        kAuxInfo[k] = {eng_.linearSampler(), aux, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkWriteDescriptorSet w[10] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aInfo, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &aUvInfo, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bInfo, nullptr, nullptr};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &bUvInfo, nullptr, nullptr};
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 4, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &pInfo, nullptr, nullptr};
    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 5, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &kInfo[0], nullptr, nullptr};
    w[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 6, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &kAuxInfo[0], nullptr, nullptr};
    w[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 7, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &kInfo[1], nullptr, nullptr};
    w[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 8, 0, 1,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &kAuxInfo[1], nullptr, nullptr};
    w[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 9, 0, 1,
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &cleanInfo, nullptr, nullptr};
    eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_.layout,
                              0, 10, w);
    CompositePC cpc{};
    cpc.aFmt = job.a->isNv12() ? 1 : 0;
    cpc.bFmt = job.b->isNv12() ? 1 : 0;
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
    auto keyPc = [&](int k, int32_t& flags, float& level, float* map,
                     int32_t& fw, int32_t& fh) {
        const GpuFrame* f = job.dsk[k] ? job.dsk[k] : job.a;
        flags = (f->isNv12() ? 1 : 0) | (f->viewA() ? 2 : 0) |
                (f->premult ? 4 : 0) |
                (f->desc.colorimetry == Colorimetry::BT601 ? 8 : 0);
        level = job.dsk[k] ? job.sw.dskLevel[k] : 0.f;
        fw = f->desc.width;
        fh = f->desc.height;
        fitMap(fw, fh, cpc.outW, cpc.outH, map);
    };
    keyPc(0, cpc.k1Flags, cpc.k1Level, cpc.k1Map, cpc.k1FullW, cpc.k1FullH);
    keyPc(1, cpc.k2Flags, cpc.k2Level, cpc.k2Map, cpc.k2FullW, cpc.k2FullH);
    cpc.cleanEnabled = job.packClean || job.packCleanNv12;
    vkCmdPushConstants(cmd, composite_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(cpc), &cpc);
    vkCmdDispatch(cmd, uint32_t((show_.width + 15) / 16),
                  uint32_t((show_.height + 15) / 16), 1);

    // program written -> read by pack + program proxy
    barrier(cmd, prog.img, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);
    if (cpc.cleanEnabled)
        barrier(cmd, clean.img, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL);

    const auto packUyvy = [&](const Image& source, Feed feed) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_.pipe);
        VkDescriptorImageInfo srcInfo{eng_.linearSampler(), source.view,
                                      VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo dstInfo{packDev_[int(feed)][fif].buf, 0,
                                       VK_WHOLE_SIZE};
        VkWriteDescriptorSet pw[2] = {};
        pw[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0,
                 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo, nullptr,
                 nullptr};
        pw[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1,
                 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dstInfo, nullptr};
        eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pack_.layout,
                                  0, 2, pw);
        PackPC ppc{show_.width, show_.height, show_.width / 2,
                   show_.colorimetry == Colorimetry::BT601 ? 1 : 0};
        vkCmdPushConstants(cmd, pack_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(ppc), &ppc);
        vkCmdDispatch(cmd, uint32_t((show_.width / 2 + 15) / 16),
                      uint32_t((show_.height + 15) / 16), 1);
    };
    if (job.packProgram) packUyvy(prog, Feed::Program);
    if (job.packClean) packUyvy(clean, Feed::Clean);

    const auto packNv12 = [&](const Image& source, Feed feed) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, packNv_.pipe);
        VkDescriptorImageInfo srcInfo{eng_.linearSampler(), source.view,
                                      VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo dstInfo{packNvDev_[int(feed)][fif].buf, 0,
                                       VK_WHOLE_SIZE};
        VkWriteDescriptorSet pw[2] = {};
        pw[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0,
                 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &srcInfo, nullptr,
                 nullptr};
        pw[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1,
                 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &dstInfo, nullptr};
        eng_.cmdPushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  packNv_.layout, 0, 2, pw);
        PackPC npc{show_.width, show_.height, 0,
                   show_.colorimetry == Colorimetry::BT601 ? 1 : 0};
        vkCmdPushConstants(cmd, packNv_.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(npc), &npc);
        vkCmdDispatch(cmd, uint32_t((show_.width / 4 + 15) / 16),
                      uint32_t((show_.height / 2 + 15) / 16), 1);
    };
    if (job.packNv12) packNv12(prog, Feed::Program);
    if (job.packCleanNv12) packNv12(clean, Feed::Clean);

    // -- proxies: program + inputs --
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, proxy_.pipe);
    int pgmProxW, pgmProxH;
    proxyUsed(show_, pgmProxW, pgmProxH);
    dispatchProxy(cmd, prog.view, VK_NULL_HANDLE, /*mode=*/1, nullptr,
                  programProxy_[fif], pgmProxW, pgmProxH);
    std::vector<std::pair<int, int>> proxDims(size_t(numInputs_), {2, 2});
    for (int i = 0; i < numInputs_; ++i) {
        const auto* f = job.mvInputs[size_t(i)].frame;
        if (!f) continue;
        int pw, ph;
        proxyUsed(f->desc, pw, ph);
        proxDims[size_t(i)] = {pw, ph};
        dispatchProxy(cmd, f->view(), f->viewUV(), f->isNv12() ? 2 : 0, &f->desc,
                      inputProxy_[fif][size_t(i)], pw, ph);
    }

    // proxies written -> sampled by tiles
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    // -- multiview: clear, tiles, labels, borders, readback --
    barrier(cmd, mv.img, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    const VkClearColorValue black{{0.f, 0.f, 0.f, 1.f}};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, mv.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    barrier(cmd, mv.img, VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tile_.pipe);
    // Live-production layout: a source matrix occupies the left half while
    // the larger PROGRAM and PREVIEW monitors stack on the right. At 16:9 the
    // output monitors are each exactly 16:9; input cells aspect-fit whatever
    // source format they receive. The input grid grows downward first so the
    // common 2-input show fills the complete left side.
    constexpr int kColumnGap = 0;
    constexpr int kOutputGap = 4;
    const int splitX = (mvW_ / 2) & ~1;
    const int inputW = splitX - kColumnGap / 2;
    const int outputX = splitX + kColumnGap / 2;
    const int outputW = mvW_ - outputX;
    const int outputH = ((mvH_ - kOutputGap) / 2) & ~1;
    const int previewY = outputH + kOutputGap;
    const int inputCols = std::clamp(numInputs_, 1, 6);
    const int inputRows = std::max(1, (numInputs_ + inputCols - 1) / inputCols);
    const int inputCellW = (inputW / inputCols) & ~1;
    const int inputVideoH = std::max(2, (inputCellW * 9 / 16) & ~1);
    const int naturalInputH = inputVideoH + kLabelRowH;
    const int inputCellH = std::min(
        naturalInputH, std::max(kLabelRowH, (mvH_ / inputRows) & ~1));

    // Pass 1: content tiles. Overlays (labels/borders) rewrite pixels these
    // dispatches produce, so each overlay pass needs a write->write barrier —
    // without it, overlapping workgroups race and borders render "dashed".
    for (int i = 0; i < numInputs_; ++i) {
        const auto& [pw, ph] = proxDims[size_t(i)];
        const int col = i % inputCols;
        const int row = i / inputCols;
        const int x = col * inputCellW;
        const int y = row * inputCellH;
        const int w = col == inputCols - 1 ? inputW - x : inputCellW;
        const int h = std::min(inputCellH, mvH_ - y);
        const int videoH = std::max(2, h - kLabelRowH);
        tileFromProxy(cmd, inputProxy_[fif][size_t(i)], pw, ph, x, y, w,
                      videoH, fif);
    }
    tileFromProxy(cmd, programProxy_[fif], pgmProxW, pgmProxH, outputX, 0,
                  outputW, outputH, fif);
    if (job.previewInputIdx >= 0) {
        const auto& [pw, ph] = proxDims[size_t(job.previewInputIdx)];
        tileFromProxy(cmd, inputProxy_[fif][size_t(job.previewInputIdx)], pw, ph,
                      outputX, previewY, outputW, mvH_ - previewY, fif);
    }

    // Pass 2: labels.
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    if (outputH >= kLabelRowH) {
        labelTile(cmd, 0, outputX, outputH - kLabelRowH, outputW, fif);
        labelTile(cmd, 1, outputX, mvH_ - kLabelRowH, outputW, fif);
    }
    for (int i = 0; i < numInputs_; ++i) {
        const int col = i % inputCols;
        const int row = i / inputCols;
        const int x = col * inputCellW;
        const int y = row * inputCellH;
        const int w = col == inputCols - 1 ? inputW - x : inputCellW;
        const int h = std::min(inputCellH, mvH_ - y);
        if (h >= kLabelRowH)
            labelTile(cmd, 2 + i, x, y + h - kLabelRowH, w, fif);
    }

    // Pass 3: tally borders (win over labels at the 3px edges).
    memBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    borderTiles(cmd, outputX, 0, outputW, outputH, kTallyRed, fif);
    borderTiles(cmd, outputX, previewY, outputW, mvH_ - previewY, kTallyGreen,
                fif);
    for (int i = 0; i < numInputs_; ++i) {
        const int col = i % inputCols;
        const int row = i / inputCols;
        const int x = col * inputCellW;
        const int y = row * inputCellH;
        const int w = col == inputCols - 1 ? inputW - x : inputCellW;
        const int h = std::min(inputCellH, mvH_ - y);
        if (i == job.tallyPgmA || i == job.tallyPgmB ||
            i == job.tallyDsk[0] || i == job.tallyDsk[1])
            borderTiles(cmd, x, y, w, h, kTallyRed, fif);
        else if (i == job.tallyPvw)
            borderTiles(cmd, x, y, w, h, kTallyGreen, fif);
    }

    // tiles written -> copy out
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

void Compositor::recordDownCopy(VkCommandBuffer cmd, int fif, int packSlot,
                                Feed feed) {
    VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
    region.size = show_.frameBytes();
    VkCopyBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
    ci.srcBuffer = packDev_[int(feed)][fif].buf;
    ci.dstBuffer = packHost_[int(feed)][packSlot].buf;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBuffer2(cmd, &ci);
}

}  // namespace moo::gpu
