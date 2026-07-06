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
