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
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace moo {

// Single-producer / single-consumer ring. Capacity rounds up to a power of two.
template <typename T>
class SpscRing {
public:
    explicit SpscRing(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    bool push(T v) {
        const uint64_t h = head_.load(std::memory_order_relaxed);
        const uint64_t t = tail_.load(std::memory_order_acquire);
        if (h - t > mask_) return false;  // full
        buf_[h & mask_] = std::move(v);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const uint64_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;  // empty
        out = std::move(buf_[t & mask_]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return size_t(head_.load(std::memory_order_acquire) -
                      tail_.load(std::memory_order_acquire));
    }
    size_t capacity() const { return mask_ + 1; }

private:
    std::vector<T> buf_;
    size_t mask_ = 0;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// Latest-value mailbox: producer overwrites, consumer reads the newest.
// The sequence number increments per publish; consumers detect drops from
// gaps and re-reads from equality. The last kKeep publishes are retained so
// a consumer that finds the newest not-yet-usable (e.g. GPU upload still in
// flight) can fall back a publish or two instead of taking nothing --
// without this, a marginal producer/consumer phase turns into a steady
// repeat+skip judder (one frame shown twice, the next never shown), and at
// razor phases even one frame back can still be mid-DMA.
// Mutex is fine at frame cadence: the critical section is a small copy.
template <typename T>
class LatestMailbox {
public:
    static constexpr int kKeep = 3;

    struct Item {
        T value;
        uint64_t seq;
    };

    void publish(T v) {
        std::lock_guard lk(m_);
        for (int k = kKeep - 1; k > 0; --k) hist_[k] = std::move(hist_[k - 1]);
        hist_[0] = std::move(v);
        ++seq_;
    }

    // Newest value if newer than lastSeq; nullopt if unchanged or never published.
    std::optional<Item> takeNewer(uint64_t lastSeq) const {
        std::lock_guard lk(m_);
        if (seq_ == 0 || seq_ == lastSeq) return std::nullopt;
        return Item{hist_[0], seq_};
    }

    // Up to kKeep retained publishes newer than lastSeq, newest first.
    // out must have room for kKeep items; returns the count written.
    int takeNewerCandidates(uint64_t lastSeq, Item* out) const {
        std::lock_guard lk(m_);
        if (seq_ == 0 || seq_ == lastSeq) return 0;
        int n = 0;
        for (int k = 0; k < kKeep; ++k) {
            if (seq_ < uint64_t(k) + 1) break;
            const uint64_t s = seq_ - uint64_t(k);
            if (s == lastSeq) break;
            out[n++] = Item{hist_[k], s};
        }
        return n;
    }

    uint64_t seq() const {
        std::lock_guard lk(m_);
        return seq_;
    }

private:
    mutable std::mutex m_;
    T hist_[kKeep]{};
    uint64_t seq_ = 0;
};

}  // namespace moo
