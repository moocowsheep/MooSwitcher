#pragma once
#include <cstddef>
#include <cstdint>

namespace moo {

enum class PixFmt : uint8_t { UYVY8_422 = 0, NV12 = 1 };  // P216 lands in v1.1
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
    size_t frameBytes() const {
        return pixfmt == PixFmt::NV12 ? size_t(width) * height * 3 / 2
                                      : size_t(width) * height * 2;
    }
    size_t rowBytes() const { return size_t(width) * 2; }  // UYVY packed rows

    // SD content is overwhelmingly BT.601; HD+ is BT.709. NDI does not signal
    // colorimetry per frame, so the standard heuristic applies.
    static Colorimetry colorimetryForHeight(int h) {
        return h < 720 ? Colorimetry::BT601 : Colorimetry::BT709;
    }
};

}  // namespace moo
