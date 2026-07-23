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
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

// End-to-end GPU golden test: synthetic UYVY -> UploadRing -> composite ->
// multiview tile -> readback, asserting converted RGB values. Runs on any
// machine with a Vulkan device; skips otherwise.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

#include "common/pattern.h"
#include "gpu/Compositor.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"

using namespace moo;

namespace {

struct GpuFixture {
    gpu::VkEngine eng;
    bool ok = false;
    GpuFixture() { ok = eng.init(false); }
};

bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

}  // namespace

TEST_CASE("composite + multiview convert BT.709 UYVY to expected RGB") {
    GpuFixture fx;
    if (!fx.ok) SKIP("no Vulkan device");
    auto& eng = fx.eng;

    VideoFormatDesc d;
    d.width = 64;
    d.height = 36;
    d.colorimetry = Colorimetry::BT709;

    auto ring = std::make_shared<gpu::UploadRing>(eng, d, eng.xferUp());
    const int slot = ring->acquire();
    REQUIRE(slot >= 0);
    // 75% red in BT.709: Y=51 U=109 V=212 -> RGB approx (191, 0, 0).
    pattern::fillRectUYVY(ring->stagingPtr(slot), int(d.rowBytes()), 0, 0, d.width,
                          d.height, 51, 109, 212);
    const uint64_t upVal = ring->submit(slot);
    REQUIRE(ring->timeline().waitCompleted(upVal, 1'000'000'000));

    auto frame = std::make_shared<const gpu::GpuFrame>(ring, slot, upVal);

    const int mvW = 64, mvH = 36;
    gpu::Compositor comp(eng, d, mvW, mvH, 1);

    gpu::Compositor::TickJob tj;
    tj.a = frame.get();
    tj.b = frame.get();
    tj.tallyPgmA = 0;  // exercises the red border path
    tj.packProgram = true;
    tj.mvInputs.push_back({frame.get()});

    VkCommandPool pool = eng.createCommandPool(eng.gfx().family);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(eng.device(), &cai, &cmd);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &bi);
    comp.record(cmd, tj, 0, 0);
    vkEndCommandBuffer(cmd);

    gpu::Timeline tl = eng.createTimeline();
    const uint64_t v = tl.reserve();
    const VkSemaphoreSubmitInfo sig =
        gpu::VkEngine::timelineSignal(tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    gpu::VkEngine::SubmitDesc sd;
    sd.cmd = cmd;
    sd.signalInfos = {&sig, 1};
    REQUIRE(eng.submit(eng.gfx(), sd) == VK_SUCCESS);
    REQUIRE(tl.waitCompleted(v, 2'000'000'000));

    // Pack readback: chained second submission (semaphore = memory visibility).
    VkCommandBuffer cmd2 = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(eng.device(), &cai, &cmd2);
    vkBeginCommandBuffer(cmd2, &bi);
    comp.recordDownCopy(cmd2, 0, 0);
    vkEndCommandBuffer(cmd2);
    const uint64_t v2 = tl.reserve();
    const VkSemaphoreSubmitInfo wait2 =
        gpu::VkEngine::timelineWait(tl, v, VK_PIPELINE_STAGE_2_COPY_BIT);
    const VkSemaphoreSubmitInfo sig2 =
        gpu::VkEngine::timelineSignal(tl, v2, VK_PIPELINE_STAGE_2_COPY_BIT);
    gpu::VkEngine::SubmitDesc sd2;
    sd2.cmd = cmd2;
    sd2.waits = {&wait2, 1};
    sd2.signalInfos = {&sig2, 1};
    REQUIRE(eng.submit(eng.gfx(), sd2) == VK_SUCCESS);
    REQUIRE(tl.waitCompleted(v2, 2'000'000'000));

    // UYVY pack roundtrip: flat 75% red must come back as (51,109,212) +-4.
    {
        const uint8_t* uyvy = comp.packPtr(0);
        const size_t off = (size_t(d.height / 2) * d.width + d.width / 2) * 2 & ~3ULL;
        CHECK(near(uyvy[off + 0], 109, 4));  // U
        CHECK(near(uyvy[off + 1], 51, 4));   // Y0
        CHECK(near(uyvy[off + 2], 212, 4));  // V
        CHECK(near(uyvy[off + 3], 51, 4));   // Y1
    }

    const uint8_t* rb = comp.readbackPtr(0);
    auto px = [&](int x, int y) { return rb + (size_t(y) * mvW + x) * 4; };

    // PROGRAM tile: upper-right quarter.
    {
        const uint8_t* p = px(48, 9);
        INFO(int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
        CHECK(near(p[0], 191, 12));
        CHECK(near(p[1], 0, 12));
        CHECK(near(p[2], 0, 12));
    }
    // PREVIEW tile: lower-right quarter.
    {
        const uint8_t* p = px(48, 27);
        CHECK(near(p[0], 191, 12));
        CHECK(near(p[1], 0, 12));
    }
    // Input matrix: one source fills the left half.
    {
        const uint8_t* p = px(12, 7);
        CHECK(near(p[0], 191, 12));
    }

    vkDestroyCommandPool(eng.device(), pool, nullptr);
    eng.destroyTimeline(tl);
    frame.reset();
    ring.reset();
}

TEST_CASE("UYVA upload ring carries the alpha plane") {
    GpuFixture fx;
    if (!fx.ok) SKIP("no Vulkan device");
    auto& eng = fx.eng;

    VideoFormatDesc d;
    d.width = 64;
    d.height = 36;
    d.pixfmt = PixFmt::UYVA8_4224;
    REQUIRE(d.frameBytes() == size_t(64) * 36 * 3);
    REQUIRE(d.alphaOffset() == size_t(64) * 2 * 36);

    auto ring = std::make_shared<gpu::UploadRing>(eng, d, eng.xferUp());
    REQUIRE(ring->hasAlpha());
    const int slot = ring->acquire();
    REQUIRE(slot >= 0);
    pattern::fillRectUYVY(ring->stagingPtr(slot), int(d.rowBytes()), 0, 0,
                          d.width, d.height, 51, 109, 212);
    memset(ring->stagingPtr(slot) + d.alphaOffset(), 0x80,
           size_t(d.width) * d.height);
    const uint64_t upVal = ring->submit(slot);
    REQUIRE(ring->timeline().waitCompleted(upVal, 1'000'000'000));

    auto frame =
        std::make_shared<const gpu::GpuFrame>(ring, slot, upVal, true);
    CHECK(frame->viewA() != VK_NULL_HANDLE);
    CHECK(frame->viewUV() == VK_NULL_HANDLE);
    CHECK(frame->premult);
    CHECK(frame->desc.hasAlpha());

    // Plain UYVY ring: no alpha view, premult defaults false.
    VideoFormatDesc d2 = d;
    d2.pixfmt = PixFmt::UYVY8_422;
    auto ring2 = std::make_shared<gpu::UploadRing>(eng, d2, eng.xferUp());
    REQUIRE_FALSE(ring2->hasAlpha());
    const int slot2 = ring2->acquire();
    REQUIRE(slot2 >= 0);
    pattern::fillRectUYVY(ring2->stagingPtr(slot2), int(d2.rowBytes()), 0, 0,
                          d2.width, d2.height, 51, 109, 212);
    const uint64_t upVal2 = ring2->submit(slot2);
    REQUIRE(ring2->timeline().waitCompleted(upVal2, 1'000'000'000));
    auto frame2 = std::make_shared<const gpu::GpuFrame>(ring2, slot2, upVal2);
    CHECK(frame2->viewA() == VK_NULL_HANDLE);
    CHECK_FALSE(frame2->premult);
}

TEST_CASE("DSK keying: straight, premult, opaque fallback, FTB over keyers") {
    GpuFixture fx;
    if (!fx.ok) SKIP("no Vulkan device");
    auto& eng = fx.eng;

    VideoFormatDesc d;
    d.width = 64;
    d.height = 36;
    d.colorimetry = Colorimetry::BT709;

    // Program bus: flat 75% red (RGB ~(191,0,0)).
    auto ringA = std::make_shared<gpu::UploadRing>(eng, d, eng.xferUp());
    const int slotA = ringA->acquire();
    pattern::fillRectUYVY(ringA->stagingPtr(slotA), int(d.rowBytes()), 0, 0,
                          d.width, d.height, 51, 109, 212);
    const uint64_t vA = ringA->submit(slotA);
    REQUIRE(ringA->timeline().waitCompleted(vA, 1'000'000'000));
    auto progFrame = std::make_shared<const gpu::GpuFrame>(ringA, slotA, vA);

    // Key: flat 75% green (RGB ~(0,191,0)) UYVA, alpha 128 everywhere.
    VideoFormatDesc dk = d;
    dk.pixfmt = PixFmt::UYVA8_4224;
    auto ringK = std::make_shared<gpu::UploadRing>(eng, dk, eng.xferUp());
    const int slotK = ringK->acquire();
    pattern::fillRectUYVY(ringK->stagingPtr(slotK), int(dk.rowBytes()), 0, 0,
                          dk.width, dk.height, 133, 63, 52);
    memset(ringK->stagingPtr(slotK) + dk.alphaOffset(), 128,
           size_t(dk.width) * dk.height);
    const uint64_t vK = ringK->submit(slotK);
    REQUIRE(ringK->timeline().waitCompleted(vK, 1'000'000'000));
    auto keyStraight = std::make_shared<const gpu::GpuFrame>(ringK, slotK, vK);
    auto keyPremult =
        std::make_shared<const gpu::GpuFrame>(ringK, slotK, vK, true);

    // No-alpha "key": plain UYVY green.
    auto ringG = std::make_shared<gpu::UploadRing>(eng, d, eng.xferUp());
    const int slotG = ringG->acquire();
    pattern::fillRectUYVY(ringG->stagingPtr(slotG), int(d.rowBytes()), 0, 0,
                          d.width, d.height, 133, 63, 52);
    const uint64_t vG = ringG->submit(slotG);
    REQUIRE(ringG->timeline().waitCompleted(vG, 1'000'000'000));
    auto keyOpaque = std::make_shared<const gpu::GpuFrame>(ringG, slotG, vG);

    const int mvW = 64, mvH = 36;
    gpu::Compositor comp(eng, d, mvW, mvH, 1);

    VkCommandPool pool = eng.createCommandPool(eng.gfx().family);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(eng.device(), &cai, &cmd);
    gpu::Timeline tl = eng.createTimeline();

    auto run = [&](gpu::Compositor::TickJob& tj) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        comp.record(cmd, tj, 0, 0);
        vkEndCommandBuffer(cmd);
        const uint64_t v = tl.reserve();
        const VkSemaphoreSubmitInfo sig = gpu::VkEngine::timelineSignal(
            tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        gpu::VkEngine::SubmitDesc sd;
        sd.cmd = cmd;
        sd.signalInfos = {&sig, 1};
        REQUIRE(eng.submit(eng.gfx(), sd) == VK_SUCCESS);
        REQUIRE(tl.waitCompleted(v, 2'000'000'000));
        return comp.readbackPtr(0);
    };
    auto baseJob = [&]() {
        gpu::Compositor::TickJob tj;
        tj.a = progFrame.get();
        tj.b = progFrame.get();
        tj.mvInputs.push_back({progFrame.get()});
        return tj;
    };
    // PROGRAM tile center in the upper-right of the 64x36 multiview.
    auto pgm = [&](const uint8_t* rb) { return rb + (size_t(9) * mvW + 48) * 4; };

    {  // straight: mix(red, green, 0.502) ~= (95, 96, 0)
        auto tj = baseJob();
        tj.dsk[0] = keyStraight.get();
        tj.sw.dskLevel[0] = 1.f;
        const uint8_t* p = pgm(run(tj));
        INFO(int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
        CHECK(near(p[0], 95, 12));
        CHECK(near(p[1], 96, 12));
        CHECK(near(p[2], 0, 12));
    }
    {  // premultiplied: green + red*(1-a) ~= (95, 191, 0)
        auto tj = baseJob();
        tj.dsk[0] = keyPremult.get();
        tj.sw.dskLevel[0] = 1.f;
        const uint8_t* p = pgm(run(tj));
        INFO(int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
        CHECK(near(p[0], 95, 12));
        CHECK(near(p[1], 191, 12));
        CHECK(near(p[2], 0, 12));
    }
    {  // no-alpha source keys fully opaque -> green
        auto tj = baseJob();
        tj.dsk[1] = keyOpaque.get();
        tj.sw.dskLevel[1] = 1.f;
        const uint8_t* p = pgm(run(tj));
        INFO(int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
        CHECK(near(p[0], 0, 12));
        CHECK(near(p[1], 191, 12));
        CHECK(near(p[2], 0, 12));
    }
    {  // half level on the opaque source: mix(red, green, 0.5)
        auto tj = baseJob();
        tj.dsk[1] = keyOpaque.get();
        tj.sw.dskLevel[1] = 0.5f;
        const uint8_t* p = pgm(run(tj));
        CHECK(near(p[0], 96, 12));
        CHECK(near(p[1], 96, 12));
    }
    {  // FTB=1 with a keyer on: everything black (keyers under FTB)
        auto tj = baseJob();
        tj.dsk[0] = keyStraight.get();
        tj.sw.dskLevel[0] = 1.f;
        tj.sw.ftb = 1.f;
        const uint8_t* p = pgm(run(tj));
        CHECK(near(p[0], 0, 6));
        CHECK(near(p[1], 0, 6));
        CHECK(near(p[2], 0, 6));
    }
    {  // look-ahead: keyer off program but at full in the preview monitor
        auto tj = baseJob();
        tj.dsk[0] = keyStraight.get();
        tj.sw.dskLevel[0] = 0.f;
        tj.pvwDskLevel[0] = 1.f;
        const uint8_t* rb = run(tj);
        const uint8_t* p = pgm(rb);
        CHECK(near(p[0], 191, 12));  // program untouched
        CHECK(near(p[1], 0, 12));
        // PREVIEW tile center: preview bus red under the half-alpha green
        // key -> mix(red, green, 0.502) ~= (95, 96, 0).
        const uint8_t* q = rb + (size_t(27) * mvW + 48) * 4;
        INFO(int(q[0]) << "," << int(q[1]) << "," << int(q[2]));
        CHECK(near(q[0], 95, 12));
        CHECK(near(q[1], 96, 12));
        CHECK(near(q[2], 0, 12));
    }
    {  // look-ahead ignores FTB: preview monitor stays lit at full black
        auto tj = baseJob();
        tj.sw.ftb = 1.f;
        const uint8_t* rb = run(tj);
        const uint8_t* q = rb + (size_t(27) * mvW + 48) * 4;
        CHECK(near(q[0], 191, 12));
    }

    vkDestroyCommandPool(eng.device(), pool, nullptr);
    eng.destroyTimeline(tl);
}

TEST_CASE("clean feed pack excludes DSK graphics") {
    GpuFixture fx;
    if (!fx.ok) SKIP("no Vulkan device");
    auto& eng = fx.eng;

    VideoFormatDesc d;
    d.width = 64;
    d.height = 36;
    d.colorimetry = Colorimetry::BT709;

    auto makeFrame = [&](uint8_t y, uint8_t u, uint8_t v, bool alpha) {
        VideoFormatDesc fd = d;
        if (alpha) fd.pixfmt = PixFmt::UYVA8_4224;
        auto ring =
            std::make_shared<gpu::UploadRing>(eng, fd, eng.xferUp());
        const int slot = ring->acquire();
        REQUIRE(slot >= 0);
        pattern::fillRectUYVY(ring->stagingPtr(slot), int(fd.rowBytes()), 0,
                              0, fd.width, fd.height, y, u, v);
        if (alpha)
            memset(ring->stagingPtr(slot) + fd.alphaOffset(), 0xff,
                   size_t(fd.width) * fd.height);
        const uint64_t upload = ring->submit(slot);
        REQUIRE(ring->timeline().waitCompleted(upload, 1'000'000'000));
        return std::make_pair(
            ring, std::make_shared<const gpu::GpuFrame>(ring, slot, upload));
    };

    // Program bus red; fully opaque DSK green.
    auto [programRing, program] = makeFrame(51, 109, 212, false);
    auto [keyRing, key] = makeFrame(133, 63, 52, true);

    gpu::Compositor comp(eng, d, 64, 36, 1);
    gpu::Compositor::TickJob job;
    job.a = program.get();
    job.b = program.get();
    job.dsk[0] = key.get();
    job.sw.dskLevel[0] = 1.f;
    job.mvInputs.push_back({program.get()});
    job.packProgram = true;
    job.packClean = true;

    VkCommandPool pool = eng.createCommandPool(eng.gfx().family);
    VkCommandBufferAllocateInfo cai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 2;
    VkCommandBuffer cmd[2]{};
    vkAllocateCommandBuffers(eng.device(), &cai, cmd);
    VkCommandBufferBeginInfo bi{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};

    vkBeginCommandBuffer(cmd[0], &bi);
    comp.record(cmd[0], job, 0, 0);
    vkEndCommandBuffer(cmd[0]);
    gpu::Timeline timeline = eng.createTimeline();
    const uint64_t rendered = timeline.reserve();
    const VkSemaphoreSubmitInfo renderSignal =
        gpu::VkEngine::timelineSignal(
            timeline, rendered, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    gpu::VkEngine::SubmitDesc renderSubmit;
    renderSubmit.cmd = cmd[0];
    renderSubmit.signalInfos = {&renderSignal, 1};
    REQUIRE(eng.submit(eng.gfx(), renderSubmit) == VK_SUCCESS);

    vkBeginCommandBuffer(cmd[1], &bi);
    comp.recordDownCopy(cmd[1], 0, 0, gpu::Compositor::Feed::Program);
    comp.recordDownCopy(cmd[1], 0, 0, gpu::Compositor::Feed::Clean);
    vkEndCommandBuffer(cmd[1]);
    const uint64_t copied = timeline.reserve();
    const VkSemaphoreSubmitInfo copyWait = gpu::VkEngine::timelineWait(
        timeline, rendered, VK_PIPELINE_STAGE_2_COPY_BIT);
    const VkSemaphoreSubmitInfo copySignal = gpu::VkEngine::timelineSignal(
        timeline, copied, VK_PIPELINE_STAGE_2_COPY_BIT);
    gpu::VkEngine::SubmitDesc copySubmit;
    copySubmit.cmd = cmd[1];
    copySubmit.waits = {&copyWait, 1};
    copySubmit.signalInfos = {&copySignal, 1};
    REQUIRE(eng.submit(eng.gfx(), copySubmit) == VK_SUCCESS);
    REQUIRE(timeline.waitCompleted(copied, 2'000'000'000));

    const size_t offset =
        ((size_t(d.height / 2) * d.width + d.width / 2) * 2) & ~3ULL;
    const uint8_t* normal = comp.packPtr(
        0, gpu::Compositor::Feed::Program);
    const uint8_t* clean =
        comp.packPtr(0, gpu::Compositor::Feed::Clean);
    // Normal program contains the opaque green DSK.
    CHECK(near(normal[offset + 0], 63, 5));
    CHECK(near(normal[offset + 1], 133, 5));
    CHECK(near(normal[offset + 2], 52, 5));
    // Clean feed remains the underlying red switched mix.
    CHECK(near(clean[offset + 0], 109, 5));
    CHECK(near(clean[offset + 1], 51, 5));
    CHECK(near(clean[offset + 2], 212, 5));

    vkDestroyCommandPool(eng.device(), pool, nullptr);
    eng.destroyTimeline(timeline);
}
