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
