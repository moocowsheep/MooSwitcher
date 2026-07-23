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
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#include "media/CudaCtx.h"

#include <cstring>
#include <unistd.h>

#include "core/Log.h"

namespace moo::media {

const char* CudaCtx::errName(CUresult r) {
    const char* s = nullptr;
    cuGetErrorName(r, &s);
    return s ? s : "CUDA_ERROR_?";
}

#define CU_CHECK(call)                                             \
    do {                                                           \
        const CUresult _r = (call);                                \
        if (_r != CUDA_SUCCESS) {                                  \
            MOO_LOGE("%s failed: %s", #call, errName(_r));         \
            return false;                                          \
        }                                                          \
    } while (0)

bool CudaCtx::init(const uint8_t vkDeviceUuid[16]) {
    CU_CHECK(cuInit(0));

    int count = 0;
    CU_CHECK(cuDeviceGetCount(&count));
    if (!count) {
        MOO_LOGE("no CUDA devices");
        return false;
    }
    dev_ = 0;
    bool matched = false;
    for (int i = 0; i < count; ++i) {
        CUdevice d;
        if (cuDeviceGet(&d, i) != CUDA_SUCCESS) continue;
        CUuuid uuid{};
        if (cuDeviceGetUuid(&uuid, d) != CUDA_SUCCESS) continue;
        if (memcmp(uuid.bytes, vkDeviceUuid, 16) == 0) {
            dev_ = d;
            matched = true;
            break;
        }
    }
    if (!matched)
        MOO_LOGW("no CUDA device UUID-matched the Vulkan device; using device 0");

    CU_CHECK(cuDevicePrimaryCtxRetain(&ctx_, dev_));
    retained_ = true;
    CU_CHECK(cuCtxSetCurrent(ctx_));
    CU_CHECK(cuStreamCreate(&stream_, CU_STREAM_NON_BLOCKING));

    char name[128] = {};
    cuDeviceGetName(name, sizeof name, dev_);
    MOO_LOGI("CUDA: %s (primary context retained)", name);
    return true;
}

void CudaCtx::destroy() {
    if (stream_) {
        cuCtxSetCurrent(ctx_);
        cuStreamDestroy(stream_);
        stream_ = nullptr;
    }
    if (retained_) {
        cuDevicePrimaryCtxRelease(dev_);
        retained_ = false;
    }
    ctx_ = nullptr;
}

bool CudaCtx::importVkFd(int fd, size_t size, Imported& out) {
    makeCurrent();
    CUDA_EXTERNAL_MEMORY_HANDLE_DESC hd{};
    hd.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    hd.handle.fd = fd;
    hd.size = size;
    const CUresult r = cuImportExternalMemory(&out.extMem, &hd);
    if (r != CUDA_SUCCESS) {
        MOO_LOGE("cuImportExternalMemory failed: %s", errName(r));
        close(fd);  // import failed -> fd still ours
        return false;
    }
    CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd{};
    bd.offset = 0;
    bd.size = size;
    const CUresult r2 = cuExternalMemoryGetMappedBuffer(&out.ptr, out.extMem, &bd);
    if (r2 != CUDA_SUCCESS) {
        MOO_LOGE("cuExternalMemoryGetMappedBuffer failed: %s", errName(r2));
        cuDestroyExternalMemory(out.extMem);
        out = {};
        return false;
    }
    out.size = size;
    return true;
}

void CudaCtx::release(Imported& im) {
    if (!im.extMem) return;
    makeCurrent();
    if (im.ptr) cuMemFree(im.ptr);
    cuDestroyExternalMemory(im.extMem);
    im = {};
}

}  // namespace moo::media
