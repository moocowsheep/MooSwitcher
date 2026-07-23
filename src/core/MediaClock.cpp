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

#include "core/MediaClock.h"

#include <cerrno>
#include <ctime>

namespace moo {

static constexpr int64_t kNsPerSec = 1'000'000'000;

MediaClock::MediaClock(int64_t fpsN, int64_t fpsD) : fpsN_(fpsN), fpsD_(fpsD) {}

void MediaClock::start() { originNs_ = nowNs(); }
void MediaClock::startAt(int64_t originNs) { originNs_ = originNs; }

int64_t MediaClock::nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

int64_t MediaClock::nsForTick(int64_t n) const {
    // n * fpsD * 1e9 / fpsN, split so no intermediate overflows int64:
    // the remainder term is bounded by fpsN * fpsD * 1e9 (< 2^63 for sane rates).
    const int64_t q = n / fpsN_;
    const int64_t r = n % fpsN_;
    return originNs_ + q * fpsD_ * kNsPerSec + r * fpsD_ * kNsPerSec / fpsN_;
}

int64_t MediaClock::tickForNs(int64_t ns) const {
    const __int128 rel = __int128(ns - originNs_) * fpsN_;
    const __int128 den = __int128(fpsD_) * kNsPerSec;
    __int128 t = rel / den;
    if (rel < 0 && rel % den != 0) --t;  // floor division for pre-origin times
    // nsForTick truncates inside its remainder term, so the exact quotient can
    // disagree by one tick at boundaries; settle against nsForTick directly.
    auto ti = int64_t(t);
    while (nsForTick(ti + 1) <= ns) ++ti;
    while (nsForTick(ti) > ns) --ti;
    return ti;
}

int64_t MediaClock::frameDurationNs() const {
    return (fpsD_ * kNsPerSec + fpsN_ / 2) / fpsN_;
}

bool MediaClock::sleepUntilTick(int64_t n) const {
    const int64_t target = nsForTick(n);
    if (target <= nowNs()) return false;
    timespec ts{target / kNsPerSec, long(target % kNsPerSec)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
    return true;
}

}  // namespace moo
