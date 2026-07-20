/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
