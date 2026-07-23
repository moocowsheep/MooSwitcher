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
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>

// Minimal timestamped stderr logger. Deliberately dependency-free for M0;
// swappable later without touching call sites.
namespace moo::log {

enum class Level { Debug = 0, Info, Warn, Error };

inline Level& threshold() {
    static Level lv = Level::Info;
    return lv;
}

inline void write(Level lv, const char* fmt, ...) {
    if (lv < threshold()) return;
    static std::mutex m;
    static const char tags[] = {'D', 'I', 'W', 'E'};

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    tm tmv;
    localtime_r(&ts.tv_sec, &tmv);

    va_list ap;
    va_start(ap, fmt);
    std::lock_guard lk(m);
    fprintf(stderr, "[%02d:%02d:%02d.%03ld][%c] ", tmv.tm_hour, tmv.tm_min,
            tmv.tm_sec, ts.tv_nsec / 1'000'000, tags[int(lv)]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

}  // namespace moo::log

#define MOO_LOGD(...) ::moo::log::write(::moo::log::Level::Debug, __VA_ARGS__)
#define MOO_LOGI(...) ::moo::log::write(::moo::log::Level::Info, __VA_ARGS__)
#define MOO_LOGW(...) ::moo::log::write(::moo::log::Level::Warn, __VA_ARGS__)
#define MOO_LOGE(...) ::moo::log::write(::moo::log::Level::Error, __VA_ARGS__)
