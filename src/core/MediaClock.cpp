/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

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
