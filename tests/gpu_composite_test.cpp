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

    vkDestroyCommandPool(eng.device(), pool, nullptr);
    eng.destroyTimeline(tl);
}
