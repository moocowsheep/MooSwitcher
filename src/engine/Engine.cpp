#include "engine/Engine.h"

#include <cstring>

#include "core/Font5x7.h"
#include "core/Log.h"
#include "core/Stats.h"
#include "ndi/NdiLib.h"
#include "out/NdiOutput.h"

namespace moo {

Engine::Engine() = default;
Engine::~Engine() { stop(); }

int64_t Engine::ndiOutFrames() const { return ndiOut_ ? ndiOut_->framesSent() : 0; }

bool Engine::start(const EngineConfig& cfg) {
    cfg_ = cfg;
    if (!vk_.init(cfg.validation)) return false;
    if (!ndi::initialize()) return false;

    comp_ = std::make_unique<gpu::Compositor>(vk_, cfg_.show, cfg_.mvW, cfg_.mvH,
                                              int(cfg_.inputs.size()));
    renderTL_ = vk_.createTimeline();
    readbackTL_ = vk_.createTimeline();
    gfxPool_ = vk_.createCommandPool(vk_.gfx().family);
    downPool_ = vk_.createCommandPool(vk_.xferDown().family);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = uint32_t(cmdBufs_.size());
    cai.commandPool = gfxPool_;
    vkAllocateCommandBuffers(vk_.device(), &cai, cmdBufs_.data());
    cai.commandPool = downPool_;
    vkAllocateCommandBuffers(vk_.device(), &cai, downBufs_.data());

    if (!createPlaceholder()) return false;
    if (!buildLabelAtlas()) return false;

    finder_ = std::make_unique<NdiFinder>();
    for (size_t i = 0; i < cfg_.inputs.size(); ++i) {
        // Spread inputs across both DMA engines; serialized 66MB 8K copies on
        // one queue otherwise hold ring slots in flight long enough to starve
        // the ring.
        auto& q = (i % 2) ? vk_.xferDown() : vk_.xferUp();
        inputs_.push_back(
            std::make_unique<NdiReceiver>(vk_, q, *finder_, cfg_.inputs[i], int(i)));
    }
    if (cfg_.ndiOut)
        ndiOut_ = std::make_unique<NdiOutput>(cfg_.ndiOutName, *comp_, readbackTL_);

    clock_ = MediaClock(cfg_.show.fpsN, cfg_.show.fpsD);
    renderThread_ = std::jthread([this](std::stop_token st) { renderLoop(st); });
    started_ = true;
    MOO_LOGI("engine started: show %dx%d @ %lld/%lld, %zu inputs, mv %dx%d, ndiOut=%s",
             cfg_.show.width, cfg_.show.height, (long long)cfg_.show.fpsN,
             (long long)cfg_.show.fpsD, inputs_.size(), comp_->mvWidth(),
             comp_->mvHeight(), cfg_.ndiOut ? cfg_.ndiOutName.c_str() : "off");
    return true;
}

bool Engine::createPlaceholder() {
    VideoFormatDesc d;
    d.width = 32;
    d.height = 18;
    placeholderRing_ = std::make_shared<gpu::UploadRing>(vk_, d, vk_.xferUp());
    const int slot = placeholderRing_->acquire();
    if (slot < 0) return false;
    uint8_t* p = placeholderRing_->stagingPtr(slot);
    for (size_t i = 0; i < d.frameBytes(); i += 2) {
        p[i] = 128;     // U/V neutral
        p[i + 1] = 16;  // Y black
    }
    const uint64_t v = placeholderRing_->submit(slot);
    if (!placeholderRing_->timeline().waitCompleted(v, 1'000'000'000)) return false;
    placeholder_ = std::make_shared<const gpu::GpuFrame>(placeholderRing_, slot, v);
    return true;
}

bool Engine::buildLabelAtlas() {
    const int rowW = 512, rowH = gpu::Compositor::kLabelRowH;
    const int rows = 2 + int(cfg_.inputs.size());
    std::vector<uint8_t> pixels(size_t(rowW) * rowH * rows * 4);
    std::vector<int> used(static_cast<size_t>(rows));

    auto renderRow = [&](int row, const std::string& text) {
        used[size_t(row)] =
            font::renderLabel(text, pixels.data() + size_t(row) * rowW * rowH * 4,
                              rowW, rowH);
    };
    renderRow(0, "PGM");
    renderRow(1, "PVW");
    for (size_t i = 0; i < cfg_.inputs.size(); ++i)
        renderRow(2 + int(i), std::to_string(i + 1) + " " + cfg_.inputs[i]);

    gpu::Image atlas = vk_.createImage2D(
        uint32_t(rowW), uint32_t(rowH * rows), VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    gpu::Buffer staging = vk_.createBuffer(
        pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!atlas.img || !staging.buf) return false;
    memcpy(staging.mapped, pixels.data(), pixels.size());

    // One-shot upload on the gfx queue.
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gfxPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk_.device(), &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = atlas.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);

    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(rowW), uint32_t(rowH * rows), 1};
    VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    ci.srcBuffer = staging.buf;
    ci.dstImage = atlas.img;
    ci.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBufferToImage2(cmd, &ci);
    vkEndCommandBuffer(cmd);

    gpu::Timeline tl = vk_.createTimeline();
    const uint64_t v = tl.reserve();
    const VkSemaphoreSubmitInfo sig = gpu::VkEngine::timelineSignal(
        tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    gpu::VkEngine::SubmitDesc sd;
    sd.cmd = cmd;
    sd.signalInfos = {&sig, 1};
    vk_.submit(vk_.gfx(), sd);
    const bool ok = tl.waitCompleted(v, 2'000'000'000);
    vk_.destroyTimeline(tl);
    vk_.destroyBuffer(staging);
    vkFreeCommandBuffers(vk_.device(), gfxPool_, 1, &cmd);
    if (!ok) return false;

    comp_->setLabelAtlas(atlas, std::move(used));
    return true;
}

void Engine::stop() {
    if (!started_) return;
    renderThread_ = {};  // request stop + join
    ndiOut_.reset();     // stops sender before its buffers go away
    inputs_.clear();
    finder_.reset();
    if (renderTL_.lastReserved())
        renderTL_.waitCompleted(renderTL_.lastReserved(), 2'000'000'000);
    vkDeviceWaitIdle(vk_.device());
    for (auto& r : retention_) r.clear();
    placeholder_.reset();
    placeholderRing_.reset();
    if (gfxPool_) vkDestroyCommandPool(vk_.device(), gfxPool_, nullptr);
    if (downPool_) vkDestroyCommandPool(vk_.device(), downPool_, nullptr);
    gfxPool_ = downPool_ = VK_NULL_HANDLE;
    vk_.destroyTimeline(renderTL_);
    vk_.destroyTimeline(readbackTL_);
    comp_.reset();
    ndi::destroy();
    vk_.destroy();
    started_ = false;
    MOO_LOGI("engine stopped");
}

bool Engine::copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w,
                           int& h) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t v = mvReady_.load(std::memory_order_acquire);
        if (!v || v == lastSeq) return false;
        const int slot = int(v % gpu::Compositor::kReadbackSlots);
        out.resize(comp_->readbackBytes());
        memcpy(out.data(), comp_->readbackPtr(slot), out.size());
        if (rbStamp_[slot].load(std::memory_order_acquire) == v) {
            lastSeq = v;
            w = comp_->mvWidth();
            h = comp_->mvHeight();
            return true;
        }
    }
    return false;
}

void Engine::renderLoop(std::stop_token st) {
    const int N = int(inputs_.size());
    std::vector<std::shared_ptr<const gpu::GpuFrame>> cur(N);
    std::vector<uint64_t> seq(N, 0);
    std::vector<int64_t> lastNewTick(N, -1000000);
    const int64_t staleTicks = 2 * cfg_.show.fpsN / cfg_.show.fpsD;  // ~2s

    auto& tickCtr = Stats::counter("render.ticks");
    auto& skipCtr = Stats::counter("render.skips");
    auto& lateWaitCtr = Stats::counter("render.gpuLateWaits");
    auto& packSkipCtr = Stats::counter("out.ndi.packSlotBusy");

    uint32_t lastTallyKey = 0xFFFFFFFF;

    clock_.start();
    int64_t n = 0;

    while (!st.stop_requested()) {
        clock_.sleepUntilTick(n);

        // -- commands --
        Command c;
        while (cmds_.pop(c)) {
            switch (c.type) {
                case Command::Type::SetProgram: switcher_.setProgram(c.arg); break;
                case Command::Type::SetPreview: switcher_.setPreview(c.arg); break;
                case Command::Type::Cut: switcher_.cut(); break;
                case Command::Type::Auto: switcher_.autoTransition(n); break;
                case Command::Type::TbarBegin: switcher_.tbarBegin(); break;
                case Command::Type::TbarSet: switcher_.tbarSet(c.farg); break;
                case Command::Type::TbarEnd: switcher_.tbarEnd(); break;
                case Command::Type::FadeToBlack: switcher_.fadeToBlack(); break;
                case Command::Type::SetTransition:
                    switcher_.setTransition(TransitionType(c.arg), c.arg2, c.farg);
                    break;
            }
        }
        const CompositeJob job = switcher_.tick(n);

        {
            std::lock_guard lk(uiM_);
            ui_ = {switcher_.program(), switcher_.preview(), job.transitionActive,
                   switcher_.ftbEngaged(), job.ftb};
        }

        // -- tally to sources (both buses are hot during a transition) --
        const int tPgmA = job.programSrc;
        const int tPgmB =
            (job.transitionActive || job.alpha > 0.f) ? job.previewSrc : -1;
        const int tPvw = job.previewSrc;
        const uint32_t tallyKey = uint32_t(tPgmA + 1) |
                                  (uint32_t(tPgmB + 1) << 10) |
                                  (uint32_t(tPvw + 1) << 20);
        if (tallyKey != lastTallyKey) {
            for (int i = 0; i < N; ++i)
                inputs_[size_t(i)]->setTally(i == tPgmA || i == tPgmB, i == tPvw);
            lastTallyKey = tallyKey;
        }

        // -- refresh inputs (only completed uploads; latest-frame policy) --
        for (int i = 0; i < N; ++i) {
            if (auto item = inputs_[size_t(i)]->newer(seq[size_t(i)])) {
                if ((*item->value).uploaded()) {
                    cur[size_t(i)] = item->value;
                    seq[size_t(i)] = item->seq;
                    lastNewTick[size_t(i)] = n;
                }
            }
            if (cur[size_t(i)] && n - lastNewTick[size_t(i)] > staleTicks)
                cur[size_t(i)].reset();  // no signal -> placeholder
        }

        auto pick = [&](int idx) -> const gpu::GpuFrame* {
            if (idx >= 0 && idx < N && cur[size_t(idx)]) return cur[size_t(idx)].get();
            return placeholder_.get();
        };

        gpu::Compositor::TickJob tj;
        tj.a = pick(job.programSrc);
        tj.b = pick(job.previewSrc);
        tj.sw = job;
        tj.previewInputIdx =
            (job.previewSrc >= 0 && job.previewSrc < N) ? job.previewSrc : -1;
        tj.tallyPgmA = tPgmA;
        tj.tallyPgmB = tPgmB;
        tj.tallyPvw = tPvw;
        tj.mvInputs.resize(size_t(N));
        for (int i = 0; i < N; ++i) tj.mvInputs[size_t(i)].frame = pick(i);

        // -- frame slot: wait out the submission from 2 ticks ago --
        const uint64_t value = renderTL_.reserve();
        (void)readbackTL_.reserve();  // keep both timelines on the same numbering
        const int fif = int(value % gpu::Compositor::kFramesInFlight);
        if (fifValues_[fif]) {
            while (!renderTL_.waitCompleted(fifValues_[fif], 500'000'000)) {
                lateWaitCtr.add();
                MOO_LOGW("render: GPU >500ms behind (value %llu)",
                         (unsigned long long)fifValues_[fif]);
                if (st.stop_requested()) return;
            }
        }
        if (downValues_[fif]) {
            while (!readbackTL_.waitCompleted(downValues_[fif], 500'000'000)) {
                lateWaitCtr.add();
                if (st.stop_requested()) return;
            }
        }
        retention_[fif].clear();

        // -- pick a pack slot for the NDI output (skip if the ring is busy) --
        int packSlot = -1;
        if (ndiOut_) {
            const uint64_t rbDone = readbackTL_.completed();
            for (int s = 0; s < gpu::Compositor::kPackSlots; ++s) {
                if (comp_->packPinned(s).load(std::memory_order_acquire)) continue;
                if (comp_->packStamp(s).load(std::memory_order_acquire) > rbDone)
                    continue;  // DMA still in flight
                packSlot = s;
                break;
            }
            if (packSlot < 0) packSkipCtr.add();
        }
        tj.packProgram = packSlot >= 0;

        const int rbSlot = int(value % gpu::Compositor::kReadbackSlots);
        rbStamp_[rbSlot].store(value, std::memory_order_release);

        VkCommandBuffer cmd = cmdBufs_[fif];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        comp_->record(cmd, tj, fif, rbSlot);
        vkEndCommandBuffer(cmd);

        std::vector<VkSemaphoreSubmitInfo> waits;
        auto addWait = [&](const gpu::GpuFrame* f) {
            if (!f) return;
            for (const auto& w : waits)
                if (w.semaphore == f->ring->timeline().handle() &&
                    w.value >= f->uploadValue)
                    return;
            waits.push_back(gpu::VkEngine::timelineWait(
                f->ring->timeline(), f->uploadValue,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
        };
        addWait(tj.a);
        addWait(tj.b);
        for (auto& s : tj.mvInputs) addWait(s.frame);

        const VkSemaphoreSubmitInfo signal = gpu::VkEngine::timelineSignal(
            renderTL_, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        gpu::VkEngine::SubmitDesc sd;
        sd.cmd = cmd;
        sd.waits = waits;
        sd.signalInfos = {&signal, 1};
        if (vk_.submit(vk_.gfx(), sd) != VK_SUCCESS) MOO_LOGE("render submit failed");
        fifValues_[fif] = value;

        // -- pack readback on the down queue (chained on this render) --
        if (packSlot >= 0) {
            comp_->packStamp(packSlot).store(value, std::memory_order_release);
            VkCommandBuffer dcmd = downBufs_[fif];
            vkResetCommandBuffer(dcmd, 0);
            vkBeginCommandBuffer(dcmd, &bi);
            comp_->recordDownCopy(dcmd, fif, packSlot);
            vkEndCommandBuffer(dcmd);

            const VkSemaphoreSubmitInfo dwait = gpu::VkEngine::timelineWait(
                renderTL_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
            const VkSemaphoreSubmitInfo dsig = gpu::VkEngine::timelineSignal(
                readbackTL_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
            gpu::VkEngine::SubmitDesc dsd;
            dsd.cmd = dcmd;
            dsd.waits = {&dwait, 1};
            dsd.signalInfos = {&dsig, 1};
            if (vk_.submit(vk_.xferDown(), dsd) != VK_SUCCESS)
                MOO_LOGE("down submit failed");
            downValues_[fif] = value;
        }

        auto& keep = retention_[fif];
        for (int i = 0; i < N; ++i)
            if (cur[size_t(i)]) keep.push_back(cur[size_t(i)]);
        keep.push_back(placeholder_);

        mvReady_.store(renderTL_.completed(), std::memory_order_release);

        ticks_.fetch_add(1, std::memory_order_relaxed);
        tickCtr.add();

        int64_t next = n + 1;
        const int64_t curTick = clock_.currentTick();
        if (curTick > next) {
            skips_.fetch_add(curTick - next, std::memory_order_relaxed);
            skipCtr.add(curTick - next);
            next = curTick;
        }
        n = next;
    }
}

}  // namespace moo
