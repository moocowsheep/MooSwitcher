// The M3 gate: Vulkan exportable buffer <-> CUDA external-memory roundtrip.
// Runs before any encoder code exists; skips without a GPU/CUDA.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

#include "gpu/VkEngine.h"
#include "media/CudaCtx.h"

using namespace moo;

TEST_CASE("Vulkan->CUDA external memory roundtrips both directions") {
    gpu::VkEngine eng;
    if (!eng.init(false)) SKIP("no Vulkan device");
    if (!eng.hasExternalMemoryFd) SKIP("no VK_KHR_external_memory_fd");

    media::CudaCtx cuda;
    if (!cuda.init(eng.deviceUuid())) SKIP("no CUDA driver/device");

    constexpr size_t kSize = 1 << 20;
    gpu::Buffer dev = eng.createBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0, /*exportable=*/true);
    REQUIRE(dev.buf != VK_NULL_HANDLE);

    gpu::Buffer host = eng.createBuffer(
        kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    REQUIRE(host.mapped != nullptr);

    // Pattern in via Vulkan.
    std::vector<uint8_t> pattern(kSize);
    for (size_t i = 0; i < kSize; ++i) pattern[i] = uint8_t(i * 131 + 7);
    memcpy(host.mapped, pattern.data(), kSize);

    VkCommandPool pool = eng.createCommandPool(eng.gfx().family);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(eng.device(), &cai, &cmd);
    gpu::Timeline tl = eng.createTimeline();

    auto runCopy = [&](VkBuffer src, VkBuffer dst) {
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkBufferCopy2 region{VK_STRUCTURE_TYPE_BUFFER_COPY_2};
        region.size = kSize;
        VkCopyBufferInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2};
        ci.srcBuffer = src;
        ci.dstBuffer = dst;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyBuffer2(cmd, &ci);
        vkEndCommandBuffer(cmd);
        const uint64_t v = tl.reserve();
        const VkSemaphoreSubmitInfo sig =
            gpu::VkEngine::timelineSignal(tl, v, VK_PIPELINE_STAGE_2_COPY_BIT);
        gpu::VkEngine::SubmitDesc sd;
        sd.cmd = cmd;
        sd.signalInfos = {&sig, 1};
        REQUIRE(eng.submit(eng.gfx(), sd) == VK_SUCCESS);
        REQUIRE(tl.waitCompleted(v, 2'000'000'000));
    };
    runCopy(host.buf, dev.buf);

    // Import into CUDA and read back.
    const int fd = eng.exportMemoryFd(dev);
    REQUIRE(fd >= 0);
    media::CudaCtx::Imported im;
    REQUIRE(cuda.importVkFd(fd, kSize, im));

    std::vector<uint8_t> fromCuda(kSize, 0);
    cuda.makeCurrent();
    REQUIRE(cuMemcpyDtoH(fromCuda.data(), im.ptr, kSize) == CUDA_SUCCESS);
    REQUIRE(memcmp(fromCuda.data(), pattern.data(), kSize) == 0);

    // Reverse: CUDA writes, Vulkan reads.
    REQUIRE(cuMemsetD8(im.ptr, 0x5A, kSize) == CUDA_SUCCESS);
    REQUIRE(cuCtxSynchronize() == CUDA_SUCCESS);
    memset(host.mapped, 0, kSize);
    runCopy(dev.buf, host.buf);
    const uint8_t* rb = static_cast<const uint8_t*>(host.mapped);
    bool all5a = true;
    for (size_t i = 0; i < kSize; ++i)
        if (rb[i] != 0x5A) {
            all5a = false;
            break;
        }
    REQUIRE(all5a);

    cuda.release(im);
    vkDestroyCommandPool(eng.device(), pool, nullptr);
    eng.destroyTimeline(tl);
    eng.destroyBuffer(dev);
    eng.destroyBuffer(host);
}
