#include "engine/Engine.h"

#include <algorithm>
#include <cstring>

#include "core/Log.h"
#include "core/Stats.h"
#include "ndi/NdiLib.h"

namespace moo {

bool Engine::start(const EngineConfig& cfg) {
    cfg_ = cfg;
    if (!vk_.init(cfg.validation)) return false;
    if (!ndi::initialize()) return false;

    comp_ = std::make_unique<gpu::Compositor>(vk_, cfg_.show, cfg_.mvW, cfg_.mvH);
    renderTL_ = vk_.createTimeline();
    gfxPool_ = vk_.createCommandPool(vk_.gfx().family);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gfxPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = uint32_t(cmdBufs_.size());
    vkAllocateCommandBuffers(vk_.device(), &cai, cmdBufs_.data());

    if (!createPlaceholder()) return false;

    finder_ = std::make_unique<NdiFinder>();
    for (size_t i = 0; i < cfg_.inputs.size(); ++i)
        inputs_.push_back(std::make_unique<NdiReceiver>(vk_, vk_.xferUp(), *finder_,
                                                        cfg_.inputs[i], int(i)));

    clock_ = MediaClock(cfg_.show.fpsN, cfg_.show.fpsD);
    renderThread_ = std::jthread([this](std::stop_token st) { renderLoop(st); });
    started_ = true;
    MOO_LOGI("engine started: show %dx%d @ %lld/%lld, %zu inputs, mv %dx%d",
             cfg_.show.width, cfg_.show.height, (long long)cfg_.show.fpsN,
             (long long)cfg_.show.fpsD, inputs_.size(), comp_->mvWidth(),
             comp_->mvHeight());
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
        p[i] = 128;      // U/V neutral
        p[i + 1] = 16;   // Y black
    }
    const uint64_t v = placeholderRing_->submit(slot);
    if (!placeholderRing_->timeline().waitCompleted(v, 1'000'000'000)) return false;
    placeholder_ = std::make_shared<const gpu::GpuFrame>(placeholderRing_, slot, v);
    return true;
}

void Engine::stop() {
    if (!started_) return;
    renderThread_ = {};   // request stop + join
    inputs_.clear();      // joins capture threads; rings die with last frame refs
    finder_.reset();
    if (renderTL_.lastReserved())
        renderTL_.waitCompleted(renderTL_.lastReserved(), 2'000'000'000);
    for (auto& r : retention_) r.clear();
    placeholder_.reset();
    placeholderRing_.reset();
    if (gfxPool_) {
        vkDestroyCommandPool(vk_.device(), gfxPool_, nullptr);
        gfxPool_ = VK_NULL_HANDLE;
    }
    vk_.destroyTimeline(renderTL_);
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
        // Slot was re-stamped while copying (engine lapped us); retry once.
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
            }
        }
        const CompositeJob job = switcher_.tick(n);

        // -- refresh inputs (only completed uploads; latest-frame policy) --
        for (int i = 0; i < N; ++i) {
            if (auto item = inputs_[i]->newer(seq[i])) {
                if ((*item->value).uploaded()) {
                    cur[i] = item->value;
                    seq[i] = item->seq;
                    lastNewTick[i] = n;
                }
            }
            if (cur[i] && n - lastNewTick[i] > staleTicks) cur[i].reset();  // no signal
        }

        auto pick = [&](int idx) -> const gpu::GpuFrame* {
            if (idx >= 0 && idx < N && cur[idx]) return cur[idx].get();
            return placeholder_.get();
        };

        gpu::Compositor::TickJob tj;
        tj.a = pick(job.programSrc);
        tj.b = pick(job.previewSrc);
        tj.sw = job;
        tj.preview = pick(job.previewSrc);
        tj.mvInputs.resize(size_t(N));
        for (int i = 0; i < N; ++i) tj.mvInputs[size_t(i)].frame = pick(i);

        // -- frame slot: wait out the submission from 2 ticks ago --
        const uint64_t value = renderTL_.reserve();
        const int fif = int(value % gpu::Compositor::kFramesInFlight);
        if (fifValues_[fif]) {
            while (!renderTL_.waitCompleted(fifValues_[fif], 500'000'000)) {
                lateWaitCtr.add();
                MOO_LOGW("render: GPU >500ms behind (value %llu)",
                         (unsigned long long)fifValues_[fif]);
                if (st.stop_requested()) return;
            }
        }
        retention_[fif].clear();  // prior frame done -> release its input refs

        const int rbSlot = int(value % gpu::Compositor::kReadbackSlots);
        rbStamp_[rbSlot].store(value, std::memory_order_release);

        VkCommandBuffer cmd = cmdBufs_[fif];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        comp_->record(cmd, tj, fif, rbSlot);
        vkEndCommandBuffer(cmd);

        // GPU-side waits on the uploads we sample (already-signaled = free).
        std::vector<VkSemaphoreSubmitInfo> waits;
        auto addWait = [&](const gpu::GpuFrame* f) {
            if (!f) return;
            for (const auto& w : waits)
                if (w.semaphore == f->ring->timeline().handle() && w.value >= f->uploadValue)
                    return;
            waits.push_back(gpu::VkEngine::timelineWait(
                f->ring->timeline(), f->uploadValue,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
        };
        addWait(tj.a);
        addWait(tj.b);
        addWait(tj.preview);
        for (auto& s : tj.mvInputs) addWait(s.frame);

        const VkSemaphoreSubmitInfo signal = gpu::VkEngine::timelineSignal(
            renderTL_, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        gpu::VkEngine::SubmitDesc sd;
        sd.cmd = cmd;
        sd.waits = waits;
        sd.signalInfos = {&signal, 1};
        if (vk_.submit(vk_.gfx(), sd) != VK_SUCCESS) MOO_LOGE("render submit failed");
        fifValues_[fif] = value;

        // Retain every frame the GPU may still be reading for this submission.
        auto& keep = retention_[fif];
        for (int i = 0; i < N; ++i)
            if (cur[i]) keep.push_back(cur[i]);
        keep.push_back(placeholder_);

        // Publish the newest fully-completed readback to the GUI.
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
