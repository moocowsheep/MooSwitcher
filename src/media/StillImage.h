#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace moo::media {

// File-type inference is deliberately conservative: these are raster formats
// FFmpeg commonly decodes as one image. The decoder remains authoritative, so
// an unreadable or unsupported file fails cleanly at load time.
inline bool isStillImagePath(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash))
        return false;

    std::string extension(path.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return extension == "png" || extension == "jpg" ||
           extension == "jpeg" || extension == "webp" ||
           extension == "bmp" || extension == "tif" ||
           extension == "tiff" || extension == "tga" ||
           extension == "exr";
}

}  // namespace moo::media
