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
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "ndi/NdiLib.h"

namespace moo {

// NDI discovery thread; keeps a name/url snapshot for substring lookup.
class NdiFinder {
public:
    struct Source {
        std::string name;
        std::string url;
    };

    NdiFinder();
    ~NdiFinder();

    std::optional<Source> lookup(const std::string& lowerSubstr) const;
    std::vector<Source> snapshot() const;

private:
    void run(std::stop_token st);

    NDIlib_find_instance_t finder_ = nullptr;
    mutable std::mutex m_;
    std::vector<Source> sources_;
    std::jthread thread_;
};

}  // namespace moo
