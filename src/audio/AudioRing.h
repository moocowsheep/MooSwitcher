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
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace moo::audio {

// Single-producer / single-consumer ring of float samples with bulk
// write/read (memcpy in at most two spans). Capacity rounds up to a power of
// two. Writer drops the tail when full; reader gets what is available.
class AudioRing {
public:
    explicit AudioRing(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    // Writer side. Returns the number of samples actually written.
    size_t write(const float* src, size_t n) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        n = std::min(n, buf_.size() - size_t(h - t));
        const size_t i = size_t(h) & mask_;
        const size_t first = std::min(n, buf_.size() - i);
        std::memcpy(buf_.data() + i, src, first * sizeof(float));
        std::memcpy(buf_.data(), src + first, (n - first) * sizeof(float));
        head_.store(h + n, std::memory_order_release);
        return n;
    }

    // Reader side. Returns the number of samples actually read.
    size_t read(float* dst, size_t n) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        n = std::min(n, size_t(h - t));
        const size_t i = size_t(t) & mask_;
        const size_t first = std::min(n, buf_.size() - i);
        std::memcpy(dst, buf_.data() + i, first * sizeof(float));
        std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(float));
        tail_.store(t + n, std::memory_order_release);
        return n;
    }

    size_t fill() const {
        return size_t(head_.load(std::memory_order_acquire) -
                      tail_.load(std::memory_order_acquire));
    }
    size_t capacity() const { return buf_.size(); }

private:
    std::vector<float> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

}  // namespace moo::audio
