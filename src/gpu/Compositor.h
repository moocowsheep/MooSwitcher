/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <array>
#include <atomic>
#include <vector>

#include "core/Format.h"
#include "engine/SwitcherCore.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"

namespace moo::gpu {

// Compute pipelines + per-frame-in-flight targets:
//   composite.comp:      inputs (UYVY) -> program RGBA16F (mix/wipes/FTB);
//                        dispatched again at proxy res for the look-ahead
//                        preview monitor (preview bus + post-transition DSKs)
//   pack_uyvy.comp:      program -> UYVY words (device buffer, for NDI out)
//   proxy_down.comp:     inputs + program -> <=960x544 RGBA8 proxies
//   multiview_tile.comp: proxies/labels/solid borders -> multiview RGBA8
// plus multiview -> host readback and the pack -> host ring for the NDI
// sender (4 slots: sender pins up to 2, one in DMA flight, one writable).
class Compositor {
public:
    static constexpr int kFramesInFlight = 2;
    static constexpr int kReadbackSlots = 3;   // multiview (GUI)
    static constexpr int kPackSlots = 4;       // program UYVY (NDI out)
    static constexpr int kFeedCount = 2;
    static constexpr int kProxyW = 960, kProxyH = 544;
    static constexpr int kLabelRowH = 24;

    enum class Feed : int { Program = 0, Clean = 1 };

    Compositor(VkEngine& eng, const VideoFormatDesc& show, int mvW, int mvH,
               int numInputs);
    ~Compositor();

    struct SourceRef {
        const GpuFrame* frame = nullptr;
    };
    struct TickJob {
        const GpuFrame* a = nullptr;   // program bus source (full res)
        const GpuFrame* b = nullptr;   // preview bus source (full res)
        CompositeJob sw;
        std::vector<SourceRef> mvInputs;  // per input, frame or placeholder
        int tallyPgmA = -1, tallyPgmB = -1, tallyPvw = -1;
        // Keyer fill frames (null = keyer dark; levels/flags ride in sw)
        // and their input indices for the red multiview border (-1 = none).
        const GpuFrame* dsk[kDskCount] = {nullptr, nullptr};
        int tallyDsk[kDskCount] = {-1, -1};
        // Look-ahead preview monitor: keyer levels for the proxy-res
        // preview composite (post-next-transition state; 0 = keyer absent).
        float pvwDskLevel[kDskCount] = {0.f, 0.f};
        bool packProgram = false;         // record UYVY pack (NDI out enabled)
        bool packClean = false;           // UYVY clean-feed NDI output
        bool packNv12 = false;            // record NV12 pack (SRT out enabled)
        bool packCleanNv12 = false;       // clean-feed recorder
    };

    void record(VkCommandBuffer cmd, const TickJob& job, int fif, int rbSlot);
    // Copy pack device buffer (fif) into host pack slot; runs on xferDown.
    void recordDownCopy(VkCommandBuffer cmd, int fif, int packSlot,
                        Feed feed = Feed::Program);

    // Multiview readback (GUI).
    const uint8_t* readbackPtr(int rbSlot) const {
        return static_cast<const uint8_t*>(readback_[rbSlot].mapped);
    }
    size_t readbackBytes() const { return size_t(mvW_) * mvH_ * 4; }
    int mvWidth() const { return mvW_; }
    int mvHeight() const { return mvH_; }

    // Pack host ring (NDI sender).
    const uint8_t* packPtr(int slot, Feed feed = Feed::Program) const {
        return static_cast<const uint8_t*>(
            packHost_[int(feed)][slot].mapped);
    }
    size_t packBytes() const { return show_.frameBytes(); }
    std::atomic<uint64_t>& packStamp(int slot, Feed feed = Feed::Program) {
        return packStamp_[int(feed)][slot];
    }
    std::atomic<bool>& packPinned(int slot, Feed feed = Feed::Program) {
        return packPinned_[int(feed)][slot];
    }

    // NV12 pack buffers for SRT/recorders (exportable; importer owns fds).
    int nvPackExportFd(int fif, Feed feed = Feed::Program) {
        return eng_.exportMemoryFd(packNvDev_[int(feed)][fif]);
    }
    size_t nvPackBytes() const { return size_t(show_.width) * show_.height * 3 / 2; }

    // Label atlas: rows 0=PROGRAM, 1=PREVIEW, 2+i=input i; usedWidths in pixels.
    void setLabelAtlas(Image atlas, std::vector<int> usedWidths);

    const VideoFormatDesc& showFormat() const { return show_; }

private:
    struct TilePC;
    struct CompositePC;
    struct PackPC;
    struct ProxyPC;

    struct Pipe {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
    };
    Pipe makePipe(const uint8_t* spv, size_t size,
                  std::initializer_list<VkDescriptorType> bindings, uint32_t pcSize);
    void destroyPipe(Pipe& p);

    void barrier(VkCommandBuffer cmd, VkImage img, VkPipelineStageFlags2 srcStage,
                 VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                 VkAccessFlags2 dstAccess, VkImageLayout oldLayout,
                 VkImageLayout newLayout);
    void memBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage,
                    VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                    VkAccessFlags2 dstAccess);
    void initImageOnce(VkCommandBuffer cmd, Image& img, uint8_t& flag);

    void dispatchProxy(VkCommandBuffer cmd, VkImageView src, VkImageView srcUv,
                       int mode, const VideoFormatDesc* srcDesc, Image& dst,
                       int usedW, int usedH);
    void dispatchTile(VkCommandBuffer cmd, VkImageView src, VkImageView srcUv,
                      int mode, const float srcMap[4],
                      const VideoFormatDesc* srcDesc, int dstX, int dstY,
                      int dstW, int dstH, int fif);
    void tileFromProxy(VkCommandBuffer cmd, const Image& proxy, int usedW,
                       int usedH, int dstX, int dstY, int dstW, int dstH, int fif);
    void labelTile(VkCommandBuffer cmd, int row, int dstX, int dstY, int dstW,
                   int fif);
    void borderTiles(VkCommandBuffer cmd, int x, int y, int w, int h,
                     const float rgb[3], int fif);

    // Per-input proxy used extent for a source format.
    static void proxyUsed(const VideoFormatDesc& d, int& w, int& h);

    VkEngine& eng_;
    VideoFormatDesc show_;
    int mvW_, mvH_, numInputs_;

    Image program_[kFramesInFlight];
    Image clean_[kFramesInFlight];
    Image multiview_[kFramesInFlight];
    Image programProxy_[kFramesInFlight];
    // Look-ahead preview monitor, composited directly at proxy resolution
    // (a full-res preview pass would double composite bandwidth -- ~32 GB/s
    // at 8K; the multiview tile is the only consumer).
    Image previewMon_[kFramesInFlight];
    std::vector<Image> inputProxy_[kFramesInFlight];  // [fif][input]
    std::vector<uint8_t> proxyInit_[kFramesInFlight];
    bool targetsInit_[kFramesInFlight] = {};

    Buffer readback_[kReadbackSlots];
    Buffer packDev_[kFeedCount][kFramesInFlight];
    Buffer packNvDev_[kFeedCount][kFramesInFlight];
    Buffer packHost_[kFeedCount][kPackSlots];
    std::atomic<uint64_t> packStamp_[kFeedCount][kPackSlots]{};
    std::atomic<bool> packPinned_[kFeedCount][kPackSlots]{};

    Image labelAtlas_{};
    std::vector<int> labelUsedW_;

    Pipe composite_, tile_, pack_, packNv_, proxy_;
};

}  // namespace moo::gpu
