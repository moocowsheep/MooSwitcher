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
#include "core/Log.h"
#include "media/FfmpegNvenc.h"
#include "media/IVideoEncoder.h"
#include "media/NvencDirect.h"

namespace moo::media {

const char* encoderBackendName(EncoderBackend backend) {
    switch (backend) {
        case EncoderBackend::Ffmpeg: return "ffmpeg";
        case EncoderBackend::Direct: return "direct";
        default: return "auto";
    }
}

bool parseEncoderBackend(std::string_view text, EncoderBackend& out) {
    if (text == "auto") out = EncoderBackend::Auto;
    else if (text == "ffmpeg") out = EncoderBackend::Ffmpeg;
    else if (text == "direct") out = EncoderBackend::Direct;
    else return false;
    return true;
}

const char* encoderPresetName(EncoderPreset preset) {
    switch (preset) {
        case EncoderPreset::P1: return "p1";
        case EncoderPreset::P2: return "p2";
        case EncoderPreset::P3: return "p3";
        case EncoderPreset::P5: return "p5";
        case EncoderPreset::P6: return "p6";
        case EncoderPreset::P7: return "p7";
        case EncoderPreset::P4: return "p4";
        default: return "auto";
    }
}

bool parseEncoderPreset(std::string_view text, EncoderPreset& out) {
    if (text == "auto") {
        out = EncoderPreset::Auto;
        return true;
    }
    for (int p = int(EncoderPreset::P1); p <= int(EncoderPreset::P7); ++p) {
        if (text == encoderPresetName(EncoderPreset(p))) {
            out = EncoderPreset(p);
            return true;
        }
    }
    return false;
}

EncoderPreset resolveEncoderPreset(EncoderPreset preset,
                                   const VideoFormatDesc& show) {
    if (preset != EncoderPreset::Auto) return preset;
    // Above 4K a P4 picture takes long enough that the pack slot is still busy
    // at the render thread's next tick check, costing whole frames
    // (out.srt.fifBusySkips); P2 encodes in time and measures identical in
    // quality at these bitrates. At or below 4K nothing is time-pressured, so
    // keep the preset that holds up better if an operator starves the bitrate.
    return show.width * show.height > 3840 * 2160 ? EncoderPreset::P2
                                                  : EncoderPreset::P4;
}

std::unique_ptr<IVideoEncoder> openVideoEncoder(CudaCtx& cuda,
                                                const VideoFormatDesc& show,
                                                const EncoderConfig& cfg) {
    if (cfg.backend != EncoderBackend::Direct) {
        auto enc = std::make_unique<FfmpegNvenc>();
        if (enc->open(cuda, show, cfg)) return enc;
        if (cfg.backend == EncoderBackend::Ffmpeg) return nullptr;
        MOO_LOGW("encoder: hevc_nvenc unavailable; falling back to direct NVENC");
    }
    auto enc = std::make_unique<NvencDirect>();
    if (enc->open(cuda, show, cfg)) return enc;
    return nullptr;
}

}  // namespace moo::media
