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

#include "ndi/NdiFinder.h"

#include <algorithm>
#include <cctype>

#include "core/Log.h"

namespace moo {

namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
}  // namespace

NdiFinder::NdiFinder() {
    NDIlib_find_create_t desc{};
    desc.show_local_sources = true;
    finder_ = NDIlib_find_create_v2(&desc);
    if (!finder_) {
        MOO_LOGE("NDIlib_find_create_v2 failed");
        return;
    }
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
}

NdiFinder::~NdiFinder() {
    thread_ = {};  // request stop + join
    if (finder_) NDIlib_find_destroy(finder_);
}

void NdiFinder::run(std::stop_token st) {
    while (!st.stop_requested()) {
        NDIlib_find_wait_for_sources(finder_, 500);
        uint32_t count = 0;
        const NDIlib_source_t* list = NDIlib_find_get_current_sources(finder_, &count);
        std::vector<Source> next;
        next.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            next.push_back({list[i].p_ndi_name ? list[i].p_ndi_name : "",
                            list[i].p_url_address ? list[i].p_url_address : ""});
        std::lock_guard lk(m_);
        sources_ = std::move(next);
    }
}

std::optional<NdiFinder::Source> NdiFinder::lookup(const std::string& lowerSubstr) const {
    std::lock_guard lk(m_);
    for (const auto& s : sources_)
        if (lower(s.name).find(lowerSubstr) != std::string::npos) return s;
    return std::nullopt;
}

std::vector<NdiFinder::Source> NdiFinder::snapshot() const {
    std::lock_guard lk(m_);
    return sources_;
}

}  // namespace moo
