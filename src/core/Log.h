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
