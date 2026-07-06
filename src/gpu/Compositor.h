#pragma once
#include <vector>

#include "core/Format.h"
#include "engine/SwitcherCore.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"

namespace moo::gpu {

// Compute pipelines + per-frame-in-flight targets:
//   composite.comp:      inputs (UYVY) -> program RGBA16F
//   multiview_tile.comp: one source -> one rect of the multiview RGBA8
// plus the multiview -> host readback copy. All images stay in GENERAL.
class Compositor {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kReadbackSlots = 3;

    Compositor(VkEngine& eng, const VideoFormatDesc& show, int mvW, int mvH);
    ~Compositor();

    struct SourceRef {
        const GpuFrame* frame = nullptr;  // UYVY input (nullptr = skip tile)
    };
    struct TickJob {
        const GpuFrame* a = nullptr;  // program bus source
        const GpuFrame* b = nullptr;  // preview bus source (transition target)
        CompositeJob sw;              // alpha/trans/ftb from SwitcherCore
        std::vector<SourceRef> mvInputs;  // input row tiles, in order
        const GpuFrame* preview = nullptr;  // PVW monitor tile
    };

    // Records dispatches + readback copy into cmd for frame-in-flight `fif`,
    // targeting readback slot `rbSlot`.
    void record(VkCommandBuffer cmd, const TickJob& job, int fif, int rbSlot);

    const uint8_t* readbackPtr(int rbSlot) const {
        return static_cast<const uint8_t*>(readback_[rbSlot].mapped);
    }
    size_t readbackBytes() const { return size_t(mvW_) * mvH_ * 4; }
    int mvWidth() const { return mvW_; }
    int mvHeight() const { return mvH_; }
    const VideoFormatDesc& showFormat() const { return show_; }

private:
    struct TilePC;
    struct CompositePC;

    void createPipelines();
    void barrier(VkCommandBuffer cmd, VkImage img, VkPipelineStageFlags2 srcStage,
                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess, VkImageLayout oldLayout,
                 VkImageLayout newLayout);
    void dispatchTile(VkCommandBuffer cmd, VkImageView src, bool srcIsRgb,
                      const VideoFormatDesc* srcDesc, int dstX, int dstY,
                      int dstW, int dstH, int fif);

    VkEngine& eng_;
    VideoFormatDesc show_;
    int mvW_, mvH_;

    Image program_[kFramesInFlight];
    Image multiview_[kFramesInFlight];
    bool targetsInitialized_[kFramesInFlight] = {};
    Buffer readback_[kReadbackSlots];

    VkDescriptorSetLayout compositeDsl_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tileDsl_ = VK_NULL_HANDLE;
    VkPipelineLayout compositeLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tileLayout_ = VK_NULL_HANDLE;
    VkPipeline compositePipe_ = VK_NULL_HANDLE;
    VkPipeline tilePipe_ = VK_NULL_HANDLE;
};

}  // namespace moo::gpu
