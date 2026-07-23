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
 * runtime, and distribute the combined work. See LICENSE-EXCEPTION.md for
 * the full exception text. */

#pragma once
#include <cstddef>
#include <cstdint>

namespace moo {

// UYVA8_4224: UYVY plane followed by a full-res 8-bit alpha plane (NDI/OMT
// native alpha). P216 lands in v1.1.
enum class PixFmt : uint8_t { UYVY8_422 = 0, NV12 = 1, UYVA8_4224 = 2 };
enum class Colorimetry : uint8_t { BT709 = 0, BT601 = 1 };

struct VideoFormatDesc {
    int width = 0;
    int height = 0;
    int64_t fpsN = 60000;
    int64_t fpsD = 1001;
    PixFmt pixfmt = PixFmt::UYVY8_422;
    Colorimetry colorimetry = Colorimetry::BT709;

    bool operator==(const VideoFormatDesc&) const = default;
    bool valid() const { return width > 1 && height > 0; }
    bool hasAlpha() const { return pixfmt == PixFmt::UYVA8_4224; }
    size_t frameBytes() const {
        switch (pixfmt) {
            case PixFmt::NV12: return size_t(width) * height * 3 / 2;
            case PixFmt::UYVA8_4224: return size_t(width) * height * 3;
            default: return size_t(width) * height * 2;
        }
    }
    size_t rowBytes() const { return size_t(width) * 2; }  // UYVY packed rows
    // Staging offset of the appended alpha plane (UYVA only).
    size_t alphaOffset() const { return size_t(width) * 2 * height; }

    // SD content is overwhelmingly BT.601; HD+ is BT.709. NDI does not signal
    // colorimetry per frame, so the standard heuristic applies.
    static Colorimetry colorimetryForHeight(int h) {
        return h < 720 ? Colorimetry::BT601 : Colorimetry::BT709;
    }
};

}  // namespace moo
