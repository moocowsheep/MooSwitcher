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

#include "core/Stats.h"

#include <map>
#include <memory>
#include <mutex>

namespace moo {

namespace {
struct Registry {
    std::mutex m;
    std::map<std::string, std::unique_ptr<Stats::Counter>> counters;
};
Registry& registry() {
    static Registry r;
    return r;
}
}  // namespace

Stats::Counter& Stats::counter(const std::string& name) {
    auto& r = registry();
    std::lock_guard lk(r.m);
    auto& slot = r.counters[name];
    if (!slot) slot = std::make_unique<Counter>();
    return *slot;
}

std::vector<Stats::Sample> Stats::snapshot() {
    auto& r = registry();
    std::lock_guard lk(r.m);
    std::vector<Sample> out;
    out.reserve(r.counters.size());
    for (auto& [name, c] : r.counters) out.push_back({name, c->value()});
    return out;
}

}  // namespace moo
