#pragma once
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace moo::gpu {

// Timeline semaphore with client-side value reservation.
class Timeline {
public:
    Timeline() = default;
    Timeline(Timeline&& o) noexcept { *this = std::move(o); }
    Timeline& operator=(Timeline&& o) noexcept {
        dev_ = o.dev_;
        sem_ = o.sem_;
        next_.store(o.next_.load());
        o.sem_ = VK_NULL_HANDLE;
        return *this;
    }

    void init(VkDevice dev, VkSemaphore sem) { dev_ = dev; sem_ = sem; }
    VkSemaphore handle() const { return sem_; }
    uint64_t reserve() { return next_.fetch_add(1) + 1; }   // next value to signal
    uint64_t lastReserved() const { return next_.load(); }
    uint64_t completed() const;
    bool waitCompleted(uint64_t value, uint64_t timeoutNs) const;

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    VkSemaphore sem_ = VK_NULL_HANDLE;
    std::atomic<uint64_t> next_{0};
};

struct Queue {
    VkQueue q = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::mutex* mtx = nullptr;  // vkQueue* calls are externally synchronized
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

// Vulkan 1.3 bring-up for a compute-only engine: one graphics+compute queue,
// two dedicated-transfer queues (upload / readback), timeline semaphores,
// push descriptors, sync2. All images the engine creates live in GENERAL
// layout after first use -- one less failure class while the engine grows.
class VkEngine {
public:
    bool init(bool enableValidation);
    void destroy();
    ~VkEngine() { destroy(); }

    VkInstance instance() const { return inst_; }
    VkPhysicalDevice physical() const { return phys_; }
    VkDevice device() const { return dev_; }

    Queue& gfx() { return gfx_; }
    Queue& xferUp() { return xferUp_; }
    Queue& xferDown() { return xferDown_; }

    // Memory type with all `required` props, preferring `preferred` present
    // and `avoid` absent (WC-vs-cached staging selection).
    uint32_t memoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred = 0,
                        VkMemoryPropertyFlags avoid = 0) const;

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags required,
                        VkMemoryPropertyFlags preferred = 0,
                        VkMemoryPropertyFlags avoid = 0,
                        bool exportable = false);
    void destroyBuffer(Buffer& b);

    // New OPAQUE_FD for the buffer's memory (caller/importer owns the fd).
    // Requires exportable=true at creation and hasExternalMemoryFd.
    int exportMemoryFd(const Buffer& b);

    Image createImage2D(uint32_t w, uint32_t h, VkFormat format,
                        VkImageUsageFlags usage);
    void destroyImage(Image& i);

    Timeline createTimeline();
    void destroyTimeline(Timeline& t);

    VkCommandPool createCommandPool(uint32_t family);
    VkSampler linearSampler() const { return sampler_; }

    struct SubmitDesc {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::span<const VkSemaphoreSubmitInfo> waits = {};
        std::span<const VkSemaphoreSubmitInfo> signalInfos = {};  // "signals" collides with Qt's macro
    };
    VkResult submit(Queue& queue, const SubmitDesc& desc);

    static VkSemaphoreSubmitInfo timelineWait(const Timeline& t, uint64_t value,
                                              VkPipelineStageFlags2 stages);
    static VkSemaphoreSubmitInfo timelineSignal(const Timeline& t, uint64_t value,
                                                VkPipelineStageFlags2 stages);

    PFN_vkCmdPushDescriptorSetKHR cmdPushDescriptorSet = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    bool hasExternalMemoryFd = false;
    bool hasExternalSemaphoreFd = false;

    // Queue families that images are shared across (CONCURRENT mode).
    std::vector<uint32_t> sharedFamilies() const;
    const uint8_t* deviceUuid() const { return deviceUuid_; }

private:
    VkInstance inst_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint8_t deviceUuid_[16] = {};

    Queue gfx_, xferUp_, xferDown_;
    std::mutex gfxMtx_, upMtx_, downMtx_;
    bool validation_ = false;
};

}  // namespace moo::gpu
