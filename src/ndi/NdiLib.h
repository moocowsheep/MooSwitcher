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
 * runtime, and distribute the combined work. See LICENSE.md for the full
 * exception text. */

#pragma once

// Central include + lifecycle for the NDI SDK. Everything NDI-facing goes
// through this header so the SDK surface stays in one place.
// The SDK headers use NULL/int64_t without including <cstddef>/<cstdint>
// themselves (breaks on gcc >= 15); provide them first.
#include <cstddef>
#include <cstdint>

#include <Processing.NDI.Lib.h>

namespace moo::ndi {

// NDIlib_initialize + version log. Returns false if the CPU/SDK refuses.
bool initialize();
void destroy();

}  // namespace moo::ndi
