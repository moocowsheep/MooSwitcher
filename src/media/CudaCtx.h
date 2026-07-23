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

#pragma once
#include <cuda.h>

#include <cstddef>
#include <cstdint>

namespace moo::media {

// CUDA driver-API context shared by the NVENC bridge, NVDEC ingest, and
// Vulkan-memory imports. Wraps the device's PRIMARY context (retained) so
// FFmpeg's AVCUDADeviceContext shares one address space with our imports.
class CudaCtx {
public:
    ~CudaCtx() { destroy(); }

    // Picks the CUDA device whose UUID matches the Vulkan physical device.
    bool init(const uint8_t vkDeviceUuid[16]);
    void destroy();

    bool ok() const { return ctx_ != nullptr; }
    CUcontext ctx() const { return ctx_; }
    CUstream stream() const { return stream_; }
    void makeCurrent() const { cuCtxSetCurrent(ctx_); }

    struct Imported {
        CUexternalMemory extMem = nullptr;
        CUdeviceptr ptr = 0;
        size_t size = 0;
    };
    // Takes ownership of fd on success (per CUDA external-memory semantics).
    bool importVkFd(int fd, size_t size, Imported& out);
    void release(Imported& im);

    static const char* errName(CUresult r);

private:
    CUdevice dev_ = 0;
    CUcontext ctx_ = nullptr;
    CUstream stream_ = nullptr;
    bool retained_ = false;
};

}  // namespace moo::media
