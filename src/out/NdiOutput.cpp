#include "out/NdiOutput.h"

#include "core/Log.h"
#include "core/Stats.h"

namespace moo {

NdiOutput::NdiOutput(std::string name, gpu::Compositor& comp,
                     gpu::Timeline& readbackTL)
    : name_(std::move(name)), comp_(comp), readbackTL_(readbackTL) {
    NDIlib_send_create_t desc{};
    desc.p_ndi_name = name_.c_str();
    desc.clock_video = false;  // the render clock paces us
    desc.clock_audio = false;
    sender_ = NDIlib_send_create(&desc);
    if (!sender_) {
        MOO_LOGE("NDI out: send_create failed");
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    MOO_LOGI("NDI out: '%s' created", name_.c_str());
}

NdiOutput::~NdiOutput() {
    thread_ = {};
    if (sender_) {
        NDIlib_send_send_video_async_v2(sender_, nullptr);  // release held buffer
        NDIlib_send_destroy(sender_);
    }
}

void NdiOutput::run(std::stop_token st) {
    auto& sentCtr = Stats::counter("out.ndi.sent");
    auto& skipCtr = Stats::counter("out.ndi.droppedToLatest");

    const auto& show = comp_.showFormat();
    NDIlib_video_frame_v2_t vf{};
    vf.xres = show.width;
    vf.yres = show.height;
    vf.FourCC = NDIlib_FourCC_video_type_UYVY;
    vf.frame_rate_N = int(show.fpsN);
    vf.frame_rate_D = int(show.fpsD);
    vf.picture_aspect_ratio = 0;
    vf.frame_format_type = NDIlib_frame_format_type_progressive;
    vf.line_stride_in_bytes = show.width * 2;
    vf.timecode = NDIlib_send_timecode_synthesize;

    uint64_t lastSent = 0;
    int prevSlot = -1;

    while (!st.stop_requested()) {
        if (!readbackTL_.waitCompleted(lastSent + 1, 100'000'000)) continue;
        const uint64_t newest = readbackTL_.completed();
        if (newest <= lastSent) continue;
        if (newest > lastSent + 1) skipCtr.add(int64_t(newest - lastSent - 1));

        // Find the slot stamped with `newest` (engine stamps before submit).
        int slot = -1;
        for (int s = 0; s < gpu::Compositor::kPackSlots; ++s)
            if (comp_.packStamp(s).load(std::memory_order_acquire) == newest) {
                slot = s;
                break;
            }
        lastSent = newest;
        if (slot < 0) continue;  // engine skipped packing that tick

        comp_.packPinned(slot).store(true, std::memory_order_release);
        vf.p_data = const_cast<uint8_t*>(comp_.packPtr(slot));
        NDIlib_send_send_video_async_v2(sender_, &vf);
        // The PREVIOUS buffer is released by this call.
        if (prevSlot >= 0 && prevSlot != slot)
            comp_.packPinned(prevSlot).store(false, std::memory_order_release);
        prevSlot = slot;
        sent_.fetch_add(1, std::memory_order_relaxed);
        sentCtr.add();
    }
}

}  // namespace moo
