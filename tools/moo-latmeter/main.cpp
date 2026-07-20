/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// moo-latmeter: NDI receiver that decodes moo-testgen's strips and reports
// end-to-end latency, frame continuity, effective fps, and A/V sync offset.
//
// Latency = CLOCK_REALTIME(now) - wallclock strip (valid same-host, or across
// PTP-synced hosts). A/V offset = tone-burst onset timecode - flash frame
// timecode (sender-stamped, so it measures the NDI chain, not our receive).

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/pattern.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "ndi/NdiLib.h"

namespace pat = moo::pattern;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
    std::string source = "MooTestgen";
    double findTimeout = 10.0;
    double duration = 0;  // 0 = until signal
    std::string csvPath;
    bool quiet = false;
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--source") {
            if (const char* v = next()) o.source = v; else return false;
        } else if (a == "--find-timeout") {
            const char* v = next();
            if (!v) return false;
            o.findTimeout = atof(v);
        } else if (a == "--duration") {
            const char* v = next();
            if (!v) return false;
            o.duration = atof(v);
        } else if (a == "--csv") {
            if (const char* v = next()) o.csvPath = v; else return false;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            fprintf(stderr,
                    "usage: moo-latmeter [--source SUBSTR] [--find-timeout S]\n"
                    "                    [--duration S] [--csv PATH] [--quiet]\n");
            return false;
        }
    }
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

int64_t realtimeNs() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) return 2;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!moo::ndi::initialize()) return 1;

    // -- discover the source --
    NDIlib_find_create_t findDesc{};
    findDesc.show_local_sources = true;
    NDIlib_find_instance_t finder = NDIlib_find_create_v2(&findDesc);
    if (!finder) {
        MOO_LOGE("NDIlib_find_create_v2 failed");
        return 1;
    }

    const std::string want = lower(opt.source);
    NDIlib_source_t source{};
    bool found = false;
    const int64_t findDeadline =
        moo::MediaClock::nowNs() + int64_t(opt.findTimeout * 1e9);
    while (!g_stop && !found && moo::MediaClock::nowNs() < findDeadline) {
        NDIlib_find_wait_for_sources(finder, 500);
        uint32_t count = 0;
        const NDIlib_source_t* list = NDIlib_find_get_current_sources(finder, &count);
        for (uint32_t i = 0; i < count; ++i) {
            if (lower(list[i].p_ndi_name).find(want) != std::string::npos) {
                source = list[i];  // strings stay valid while finder lives
                found = true;
                break;
            }
        }
    }
    if (!found) {
        MOO_LOGE("no NDI source matching '%s' within %.1fs", opt.source.c_str(),
                 opt.findTimeout);
        NDIlib_find_destroy(finder);
        moo::ndi::destroy();
        return 2;
    }
    MOO_LOGI("connecting to '%s'", source.p_ndi_name);

    NDIlib_recv_create_v3_t recvDesc{};
    recvDesc.source_to_connect_to = source;
    recvDesc.color_format = NDIlib_recv_color_format_fastest;  // UYVY
    recvDesc.bandwidth = NDIlib_recv_bandwidth_highest;
    recvDesc.allow_video_fields = false;
    recvDesc.p_ndi_recv_name = "moo-latmeter";
    NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&recvDesc);
    if (!recv) {
        MOO_LOGE("NDIlib_recv_create_v3 failed");
        return 1;
    }
    NDIlib_find_destroy(finder);  // after recv holds its own copy of the source

    FILE* csv = nullptr;
    if (!opt.csvPath.empty()) {
        csv = fopen(opt.csvPath.c_str(), "w");
        if (!csv) {
            MOO_LOGE("cannot open csv '%s'", opt.csvPath.c_str());
            return 2;
        }
        fprintf(csv,
                "time_s,frames,fps,gaps,bad_parity,lat_avg_ms,lat_min_ms,"
                "lat_max_ms,av_offset_ms\n");
    }

    // -- receive loop --
    const int64_t startNs = moo::MediaClock::nowNs();
    const int64_t endNs =
        opt.duration > 0 ? startNs + int64_t(opt.duration * 1e9) : 0;

    int64_t totalFrames = 0, totalGaps = 0, totalBad = 0;
    int64_t lastCounter = -1;

    int64_t winStart = startNs, winFrames = 0, winGaps = 0, winBad = 0;
    double winLatSum = 0, winLatMin = 1e18, winLatMax = -1e18;
    int64_t winLatCount = 0;

    // A/V pairing state (sender timecodes, 100ns units).
    int64_t lastFlashT = INT64_MIN, lastOnsetT = INT64_MIN;
    double avOffsetMs = NAN;
    int quietRun = 1 << 30;  // start "quiet" so the first burst counts

    double latAvgAll = 0;
    int64_t latCountAll = 0;

    while (!g_stop && (endNs == 0 || moo::MediaClock::nowNs() < endNs)) {
        NDIlib_video_frame_v2_t vf{};
        NDIlib_audio_frame_v3_t af{};
        const NDIlib_frame_type_e ft =
            NDIlib_recv_capture_v3(recv, &vf, &af, nullptr, 500);

        if (ft == NDIlib_frame_type_video) {
            ++totalFrames;
            ++winFrames;
            const uint8_t* data = vf.p_data;
            const int stride = vf.line_stride_in_bytes;

            uint64_t counter = 0, sendNs = 0;
            const bool okC = pat::readStrip(data, stride, pat::kCounterRow, counter);
            const bool okT = pat::readStrip(data, stride, pat::kTimeRow, sendNs);
            if (!okC || !okT) {
                ++totalBad;
                ++winBad;
            } else {
                if (lastCounter >= 0 && int64_t(counter) > lastCounter + 1) {
                    const int64_t g = int64_t(counter) - lastCounter - 1;
                    totalGaps += g;
                    winGaps += g;
                }
                lastCounter = int64_t(counter);

                const double latMs = double(realtimeNs() - int64_t(sendNs)) / 1e6;
                if (latMs < -1000.0 || latMs > 10'000.0) {
                    // Blended strips can pass 8-bit parity by luck (~1/256)
                    // during transitions; discard absurd timestamps.
                    ++totalBad;
                    ++winBad;
                    NDIlib_recv_free_video_v2(recv, &vf);
                    continue;
                }
                winLatSum += latMs;
                winLatMin = std::min(winLatMin, latMs);
                winLatMax = std::max(winLatMax, latMs);
                ++winLatCount;
                latAvgAll += latMs;
                ++latCountAll;

                if (pat::readFlash(data, stride)) {
                    lastFlashT = vf.timecode;
                    if (std::abs(lastFlashT - lastOnsetT) < 5'000'000)  // 0.5 s
                        avOffsetMs = double(lastOnsetT - lastFlashT) / 1e4;
                }
            }
            NDIlib_recv_free_video_v2(recv, &vf);
        } else if (ft == NDIlib_frame_type_audio) {
            if (af.FourCC == NDIlib_FourCC_audio_type_FLTP && af.no_channels > 0) {
                // Onset detect with a hold zone: the burst is a sine ramping
                // from zero, so early samples sit between the quiet (0.05)
                // and trigger (0.15) thresholds and must not disarm us.
                const float* ch0 = reinterpret_cast<const float*>(af.p_data);
                for (int i = 0; i < af.no_samples; ++i) {
                    const float x = std::fabs(ch0[i]);
                    if (x > 0.15f) {
                        if (quietRun > pat::kSampleRate / 20) {  // >=50ms quiet
                            lastOnsetT =
                                af.timecode + int64_t(i) * 10'000'000 / pat::kSampleRate;
                            if (std::abs(lastFlashT - lastOnsetT) < 5'000'000)
                                avOffsetMs = double(lastOnsetT - lastFlashT) / 1e4;
                        }
                        quietRun = 0;
                    } else if (x < 0.05f) {
                        ++quietRun;
                    }  // else: ramp zone, hold state
                }
            }
            NDIlib_recv_free_audio_v3(recv, &af);
        }

        const int64_t nowNs = moo::MediaClock::nowNs();
        if (nowNs - winStart >= 1'000'000'000) {
            const double dt = double(nowNs - winStart) / 1e9;
            const double fps = winFrames / dt;
            const double lavg = winLatCount ? winLatSum / winLatCount : NAN;
            if (!opt.quiet)
                MOO_LOGI(
                    "fps=%6.2f frames=%lld gaps=%lld bad=%lld lat(ms) "
                    "avg=%6.2f min=%6.2f max=%6.2f av=%+.2fms",
                    fps, (long long)totalFrames, (long long)totalGaps,
                    (long long)totalBad, lavg,
                    winLatCount ? winLatMin : NAN, winLatCount ? winLatMax : NAN,
                    avOffsetMs);
            if (csv) {
                fprintf(csv, "%.3f,%lld,%.3f,%lld,%lld,%.3f,%.3f,%.3f,%.3f\n",
                        double(nowNs - startNs) / 1e9, (long long)totalFrames,
                        fps, (long long)winGaps, (long long)winBad, lavg,
                        winLatCount ? winLatMin : NAN,
                        winLatCount ? winLatMax : NAN, avOffsetMs);
                fflush(csv);
            }
            winStart = nowNs;
            winFrames = winGaps = winBad = 0;
            winLatSum = 0;
            winLatMin = 1e18;
            winLatMax = -1e18;
            winLatCount = 0;
        }
    }

    NDIlib_recv_destroy(recv);
    moo::ndi::destroy();

    MOO_LOGI("summary: frames=%lld gaps=%lld bad=%lld lat_avg=%.2fms av=%+.2fms",
             (long long)totalFrames, (long long)totalGaps, (long long)totalBad,
             latCountAll ? latAvgAll / latCountAll : NAN, avOffsetMs);
    if (csv) fclose(csv);
    return totalFrames > 0 ? 0 : 1;
}
