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
 * runtime, and distribute the combined work. See EXCEPTIONS.md for the
 * full exception text. */

#include "gpu/VkEngine.h"

#include <cstring>

#include "core/Log.h"

namespace moo::gpu {

uint64_t Timeline::completed() const {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev_, sem_, &v);
    return v;
}

bool Timeline::waitCompleted(uint64_t value, uint64_t timeoutNs) const {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &sem_;
    wi.pValues = &value;
    return vkWaitSemaphores(dev_, &wi, timeoutNs) == VK_SUCCESS;
}

bool VkEngine::init(bool enableValidation) {
    // -- instance --
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MooSwitcher";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    if (enableValidation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (auto& p : props)
            if (!strcmp(p.layerName, "VK_LAYER_KHRONOS_validation")) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                validation_ = true;
            }
        if (!validation_)
            MOO_LOGW("validation requested but VK_LAYER_KHRONOS_validation not present");
    }

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &inst_) != VK_SUCCESS) {
        MOO_LOGE("vkCreateInstance failed");
        return false;
    }

    // -- physical device: prefer a discrete GPU --
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(inst_, &devCount, nullptr);
    if (!devCount) {
        MOO_LOGE("no Vulkan devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(devCount);
    vkEnumeratePhysicalDevices(inst_, &devCount, devs.data());
    phys_ = devs[0];
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            phys_ = d;
            break;
        }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys_, &props);
    MOO_LOGI("GPU: %s (Vulkan %u.%u.%u)", props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion),
             VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);

    VkPhysicalDeviceIDProperties idProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &idProps;
    vkGetPhysicalDeviceProperties2(phys_, &props2);
    memcpy(deviceUuid_, idProps.deviceUUID, 16);

    // -- queue families: graphics+compute, plus a dedicated transfer family --
    uint32_t famCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &famCount, nullptr);
    std::vector<VkQueueFamilyProperties> fams(famCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &famCount, fams.data());

    uint32_t gfxFam = UINT32_MAX, xferFam = UINT32_MAX;
    for (uint32_t i = 0; i < famCount; ++i) {
        const auto f = fams[i].queueFlags;
        if (gfxFam == UINT32_MAX && (f & VK_QUEUE_GRAPHICS_BIT) && (f & VK_QUEUE_COMPUTE_BIT))
            gfxFam = i;
        if (xferFam == UINT32_MAX && (f & VK_QUEUE_TRANSFER_BIT) &&
            !(f & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
            xferFam = i;
    }
    if (gfxFam == UINT32_MAX) {
        MOO_LOGE("no graphics+compute queue family");
        return false;
    }
    const uint32_t xferQueues =
        xferFam == UINT32_MAX ? 0 : (fams[xferFam].queueCount >= 2 ? 2u : 1u);

    const float prio[2] = {1.f, 1.f};
    std::vector<VkDeviceQueueCreateInfo> qcis;
    VkDeviceQueueCreateInfo qg{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qg.queueFamilyIndex = gfxFam;
    qg.queueCount = 1;
    qg.pQueuePriorities = prio;
    qcis.push_back(qg);
    if (xferQueues) {
        VkDeviceQueueCreateInfo qt{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qt.queueFamilyIndex = xferFam;
        qt.queueCount = xferQueues;
        qt.pQueuePriorities = prio;
        qcis.push_back(qt);
    }

    // -- extensions & features --
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
    auto hasExt = [&](const char* n) {
        for (auto& e : exts)
            if (!strcmp(e.extensionName, n)) return true;
        return false;
    };
    std::vector<const char*> devExts;
    if (!hasExt(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
        MOO_LOGE("VK_KHR_push_descriptor unavailable");
        return false;
    }
    devExts.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if ((hasExternalMemoryFd = hasExt(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)))
        devExts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    if ((hasExternalSemaphoreFd = hasExt(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME)))
        devExts.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = uint32_t(qcis.size());
    dci.pQueueCreateInfos = qcis.data();
    dci.enabledExtensionCount = uint32_t(devExts.size());
    dci.ppEnabledExtensionNames = devExts.data();
    if (vkCreateDevice(phys_, &dci, nullptr, &dev_) != VK_SUCCESS) {
        MOO_LOGE("vkCreateDevice failed");
        return false;
    }

    gfx_ = {nullptr, gfxFam, &gfxMtx_};
    vkGetDeviceQueue(dev_, gfxFam, 0, &gfx_.q);
    if (xferQueues) {
        xferUp_ = {nullptr, xferFam, &upMtx_};
        vkGetDeviceQueue(dev_, xferFam, 0, &xferUp_.q);
        if (xferQueues >= 2) {
            xferDown_ = {nullptr, xferFam, &downMtx_};
            vkGetDeviceQueue(dev_, xferFam, 1, &xferDown_.q);
        } else {
            xferDown_ = xferUp_;  // shared queue, shared mutex
        }
    } else {
        MOO_LOGW("no dedicated transfer family; sharing graphics queue");
        xferUp_ = xferDown_ = gfx_;
    }
    MOO_LOGI("queues: gfx fam %u, xfer fam %u x%u", gfxFam,
             xferQueues ? xferFam : gfxFam, xferQueues ? xferQueues : 0);

    cmdPushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
        vkGetDeviceProcAddr(dev_, "vkCmdPushDescriptorSetKHR"));
    if (hasExternalMemoryFd)
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(
            vkGetDeviceProcAddr(dev_, "vkGetMemoryFdKHR"));

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &sci, nullptr, &sampler_);
    return true;
}

void VkEngine::destroy() {
    if (!dev_) {
        if (inst_) vkDestroyInstance(inst_, nullptr), inst_ = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(dev_);
    if (sampler_) vkDestroySampler(dev_, sampler_, nullptr), sampler_ = VK_NULL_HANDLE;
    vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
    vkDestroyInstance(inst_, nullptr);
    inst_ = VK_NULL_HANDLE;
}

uint32_t VkEngine::memoryType(uint32_t typeBits, VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred,
                              VkMemoryPropertyFlags avoid) const {
    uint32_t best = UINT32_MAX;
    int bestScore = -1;
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        const auto flags = memProps_.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) continue;
        int score = 0;
        if (preferred && (flags & preferred) == preferred) score += 2;
        if (avoid && !(flags & avoid)) score += 1;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

Buffer VkEngine::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                              VkMemoryPropertyFlags required,
                              VkMemoryPropertyFlags preferred,
                              VkMemoryPropertyFlags avoid, bool exportable) {
    Buffer b;
    b.size = size;
    VkExternalMemoryBufferCreateInfo ext{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    if (exportable) bci.pNext = &ext;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev_, &bci, nullptr, &b.buf) != VK_SUCCESS) return {};

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, b.buf, &req);
    const uint32_t type = memoryType(req.memoryTypeBits, required, preferred, avoid);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(dev_, b.buf, nullptr);
        return {};
    }
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (exportable) mai.pNext = &exp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev_, &mai, nullptr, &b.mem) != VK_SUCCESS) {
        vkDestroyBuffer(dev_, b.buf, nullptr);
        return {};
    }
    vkBindBufferMemory(dev_, b.buf, b.mem, 0);
    if (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(dev_, b.mem, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}

int VkEngine::exportMemoryFd(const Buffer& b) {
    if (!getMemoryFd) return -1;
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = b.mem;
    gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (getMemoryFd(dev_, &gi, &fd) != VK_SUCCESS) return -1;
    return fd;
}

void VkEngine::destroyBuffer(Buffer& b) {
    if (b.buf) vkDestroyBuffer(dev_, b.buf, nullptr);
    if (b.mem) vkFreeMemory(dev_, b.mem, nullptr);
    b = {};
}

std::vector<uint32_t> VkEngine::sharedFamilies() const {
    std::vector<uint32_t> fams{gfx_.family};
    if (xferUp_.family != gfx_.family) fams.push_back(xferUp_.family);
    return fams;
}

Image VkEngine::createImage2D(uint32_t w, uint32_t h, VkFormat format,
                              VkImageUsageFlags usage) {
    Image im;
    im.format = format;
    im.width = w;
    im.height = h;

    const auto fams = sharedFamilies();
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fams.size() > 1) {  // skip queue-family ownership transfers entirely
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = uint32_t(fams.size());
        ici.pQueueFamilyIndices = fams.data();
    }
    if (vkCreateImage(dev_, &ici, nullptr, &im.img) != VK_SUCCESS) return {};

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, im.img, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &mai, nullptr, &im.mem) != VK_SUCCESS) {
        vkDestroyImage(dev_, im.img, nullptr);
        return {};
    }
    vkBindImageMemory(dev_, im.img, im.mem, 0);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(dev_, &vci, nullptr, &im.view);
    return im;
}

void VkEngine::destroyImage(Image& i) {
    if (i.view) vkDestroyImageView(dev_, i.view, nullptr);
    if (i.img) vkDestroyImage(dev_, i.img, nullptr);
    if (i.mem) vkFreeMemory(dev_, i.mem, nullptr);
    i = {};
}

Timeline VkEngine::createTimeline() {
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    tci.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &tci;
    VkSemaphore sem = VK_NULL_HANDLE;
    vkCreateSemaphore(dev_, &sci, nullptr, &sem);
    Timeline t;
    t.init(dev_, sem);
    return t;
}

void VkEngine::destroyTimeline(Timeline& t) {
    if (t.handle()) vkDestroySemaphore(dev_, t.handle(), nullptr);
    t.init(dev_, VK_NULL_HANDLE);
}

VkCommandPool VkEngine::createCommandPool(uint32_t family) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev_, &pci, nullptr, &pool);
    return pool;
}

VkResult VkEngine::submit(Queue& queue, const SubmitDesc& desc) {
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = desc.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = uint32_t(desc.waits.size());
    si.pWaitSemaphoreInfos = desc.waits.data();
    si.commandBufferInfoCount = desc.cmd ? 1u : 0u;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = uint32_t(desc.signalInfos.size());
    si.pSignalSemaphoreInfos = desc.signalInfos.data();
    std::lock_guard lk(*queue.mtx);
    return vkQueueSubmit2(queue.q, 1, &si, VK_NULL_HANDLE);
}

VkSemaphoreSubmitInfo VkEngine::timelineWait(const Timeline& t, uint64_t value,
                                             VkPipelineStageFlags2 stages) {
    VkSemaphoreSubmitInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    si.semaphore = t.handle();
    si.value = value;
    si.stageMask = stages;
    return si;
}

VkSemaphoreSubmitInfo VkEngine::timelineSignal(const Timeline& t, uint64_t value,
                                               VkPipelineStageFlags2 stages) {
    return timelineWait(t, value, stages);
}

}  // namespace moo::gpu
