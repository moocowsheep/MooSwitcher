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
    tj.previewInputIdx = 0;
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

    // PGM tile: left half (32x24), 64x36 fits to 32x18 centered (3px bars).
    {
        const uint8_t* p = px(16, 12);
        INFO(int(p[0]) << "," << int(p[1]) << "," << int(p[2]));
        CHECK(near(p[0], 191, 12));
        CHECK(near(p[1], 0, 12));
        CHECK(near(p[2], 0, 12));
    }
    // PVW tile: right half center.
    {
        const uint8_t* p = px(48, 12);
        CHECK(near(p[0], 191, 12));
        CHECK(near(p[1], 0, 12));
    }
    // Letterbox bar inside PGM tile (top rows of the tile) stays black.
    {
        const uint8_t* p = px(16, 1);
        CHECK(near(p[0], 0, 6));
        CHECK(near(p[1], 0, 6));
        CHECK(near(p[2], 0, 6));
    }
    // Input row cell 0 center.
    {
        const int rowY = (mvH * 2 / 3) & ~1;  // 24
        const uint8_t* p = px(8, rowY + (mvH - rowY) / 2);
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
