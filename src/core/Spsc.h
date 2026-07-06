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
// gaps and re-reads from equality. Mutex is fine at frame cadence: the
// critical section is one small move/copy.
template <typename T>
class LatestMailbox {
public:
    struct Item {
        T value;
        uint64_t seq;
    };

    void publish(T v) {
        std::lock_guard lk(m_);
        slot_ = std::move(v);
        ++seq_;
    }

    // Newest value if newer than lastSeq; nullopt if unchanged or never published.
    std::optional<Item> takeNewer(uint64_t lastSeq) const {
        std::lock_guard lk(m_);
        if (seq_ == 0 || seq_ == lastSeq) return std::nullopt;
        return Item{slot_, seq_};
    }

    uint64_t seq() const {
        std::lock_guard lk(m_);
        return seq_;
    }

private:
    mutable std::mutex m_;
    T slot_{};
    uint64_t seq_ = 0;
};

}  // namespace moo
