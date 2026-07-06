// moo-testgen: NDI test-pattern sender with machine-readable latency strips.
//
// Emits UYVY color bars with a moving bar, a 64-bit frame counter strip, a
// send-wallclock strip (CLOCK_REALTIME ns), a flash region + 1 kHz tone burst
// every 60 frames (A/V sync measurement), and stereo FLTP audio.
// Paced by MediaClock (rational fps, no drift); video is sent with
// NDIlib_send_send_video_async_v2 over a ring of precomputed frames.

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/pattern.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "ndi/NdiLib.h"

namespace pat = moo::pattern;

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

struct Options {
    std::string name = "MooTestgen";
    int width = 1920;
    int height = 1080;
    int64_t fpsN = 60000;
    int64_t fpsD = 1001;
    int precompute = 0;  // 0 = auto
    double duration = 0;  // seconds, 0 = until signal
    bool audio = true;
    bool quiet = false;
};

bool parseFps(const std::string& s, int64_t& n, int64_t& d) {
    if (const auto slash = s.find('/'); slash != std::string::npos) {
        n = atoll(s.c_str());
        d = atoll(s.c_str() + slash + 1);
        return n > 0 && d > 0;
    }
    const double f = atof(s.c_str());
    if (f <= 0) return false;
    const double r = std::round(f);
    if (std::abs(f - r) < 0.005) {  // integer rate
        n = int64_t(r);
        d = 1;
    } else {  // NTSC-style fractional rate
        n = int64_t(r) * 1000;
        d = 1001;
    }
    return true;
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--name") {
            if (const char* v = next()) o.name = v; else return false;
        } else if (a == "--size") {
            const char* v = next();
            if (!v || sscanf(v, "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (a == "--fps") {
            const char* v = next();
            if (!v || !parseFps(v, o.fpsN, o.fpsD)) return false;
        } else if (a == "--precompute") {
            const char* v = next();
            if (!v) return false;
            o.precompute = atoi(v);
        } else if (a == "--duration") {
            const char* v = next();
            if (!v) return false;
            o.duration = atof(v);
        } else if (a == "--no-audio") {
            o.audio = false;
        } else if (a == "--quiet") {
            o.quiet = true;
        } else {
            fprintf(stderr,
                    "usage: moo-testgen [--name S] [--size WxH] [--fps N/D|F]\n"
                    "                   [--precompute K] [--duration SECS]\n"
                    "                   [--no-audio] [--quiet]\n");
            return false;
        }
    }
    return true;
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
    if (opt.width < 1280 || opt.height < 128 || (opt.width & 1)) {
        MOO_LOGE("size must be even-width, >=1280x128 (strips need the room)");
        return 2;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!moo::ndi::initialize()) return 1;

    NDIlib_send_create_t sendDesc{};
    sendDesc.p_ndi_name = opt.name.c_str();
    sendDesc.clock_video = false;  // we pace with MediaClock
    sendDesc.clock_audio = false;
    NDIlib_send_instance_t sender = NDIlib_send_create(&sendDesc);
    if (!sender) {
        MOO_LOGE("NDIlib_send_create failed");
        return 1;
    }

    const int strideBytes = opt.width * 2;
    const size_t frameBytes = size_t(strideBytes) * opt.height;

    // Precomputed frame ring: bars + moving bar baked per slot; strips and the
    // flash region get stamped per send. K >= 2 keeps the async-held buffer
    // (previous frame) untouched while we stamp the current slot.
    int K = opt.precompute;
    if (K <= 0) K = int(1'500'000'000 / frameBytes);
    K = std::max(2, std::min(K, 60));
    if (!opt.quiet)
        MOO_LOGI("precomputing %d x %dx%d UYVY frames (%.1f MB)", K, opt.width,
                 opt.height, double(frameBytes) * K / 1e6);

    std::vector<std::vector<uint8_t>> slots(K);
    for (int i = 0; i < K; ++i) {
        slots[i].resize(frameBytes);
        pat::fillBars(slots[i].data(), strideBytes, opt.width, opt.height);
        pat::bakeMovingBar(slots[i].data(), strideBytes, opt.width, opt.height, i, K);
    }

    // Audio: stereo FLTP, chunk per video tick, tone burst after each flash.
    const int64_t flashPeriodSamples =
        pat::sampleForTick(pat::kFlashPeriodTicks, opt.fpsN, opt.fpsD);
    std::vector<float> audioBuf;  // [ch0 samples][ch1 samples]

    moo::MediaClock clock(opt.fpsN, opt.fpsD);
    moo::MediaClock ideal(opt.fpsN, opt.fpsD);  // origin 0: ideal timeline
    ideal.startAt(0);

    const int64_t t0Real = realtimeNs();
    clock.start();

    NDIlib_video_frame_v2_t vf{};
    vf.xres = opt.width;
    vf.yres = opt.height;
    vf.FourCC = NDIlib_FourCC_video_type_UYVY;
    vf.frame_rate_N = int(opt.fpsN);
    vf.frame_rate_D = int(opt.fpsD);
    vf.picture_aspect_ratio = 0;  // square pixels
    vf.frame_format_type = NDIlib_frame_format_type_progressive;
    vf.line_stride_in_bytes = strideBytes;

    const int64_t maxTicks =
        opt.duration > 0 ? int64_t(opt.duration * opt.fpsN / opt.fpsD) + 1 : 0;

    int64_t n = 0, sent = 0, skipped = 0, samplesSent = 0;
    int64_t windowStartNs = moo::MediaClock::nowNs(), windowFrames = 0;

    MOO_LOGI("sending '%s' %dx%d @ %lld/%lld%s", opt.name.c_str(), opt.width,
             opt.height, (long long)opt.fpsN, (long long)opt.fpsD,
             opt.audio ? " with audio" : "");

    while (!g_stop && (maxTicks == 0 || n < maxTicks)) {
        clock.sleepUntilTick(n);

        uint8_t* frame = slots[size_t(n % K)].data();
        pat::stampStrip(frame, strideBytes, pat::kCounterRow, uint64_t(n));
        pat::stampStrip(frame, strideBytes, pat::kTimeRow, uint64_t(realtimeNs()));
        pat::stampFlash(frame, strideBytes, n % pat::kFlashPeriodTicks == 0);

        vf.p_data = frame;
        vf.timecode = (t0Real + ideal.nsForTick(n)) / 100;  // 100ns units
        NDIlib_send_send_video_async_v2(sender, &vf);  // blocks only if encoder is behind
        ++sent;
        ++windowFrames;

        if (opt.audio) {
            const int64_t s0 = pat::sampleForTick(n, opt.fpsN, opt.fpsD);
            const int64_t s1 = pat::sampleForTick(n + 1, opt.fpsN, opt.fpsD);
            const int count = int(s1 - s0);
            audioBuf.resize(size_t(count) * 2);
            for (int i = 0; i < count; ++i) {
                const float v = pat::toneSample(s0 + i, flashPeriodSamples);
                audioBuf[size_t(i)] = v;
                audioBuf[size_t(count + i)] = v;
            }
            NDIlib_audio_frame_v3_t af{};
            af.sample_rate = pat::kSampleRate;
            af.no_channels = 2;
            af.no_samples = count;
            af.timecode = (t0Real + s0 * 1'000'000'000 / pat::kSampleRate) / 100;
            af.FourCC = NDIlib_FourCC_audio_type_FLTP;
            af.p_data = reinterpret_cast<uint8_t*>(audioBuf.data());
            af.channel_stride_in_bytes = count * int(sizeof(float));
            NDIlib_send_send_audio_v3(sender, &af);
            samplesSent += count;
        }

        // Advance; if the encoder held us past deadlines, skip (and count) ticks.
        int64_t next = n + 1;
        const int64_t cur = clock.currentTick();
        if (cur > next) {
            skipped += cur - next;
            next = cur;
        }
        n = next;

        const int64_t nowNs = moo::MediaClock::nowNs();
        if (!opt.quiet && nowNs - windowStartNs >= 5'000'000'000) {
            MOO_LOGI("sent=%lld fps=%.2f skipped=%lld", (long long)sent,
                     windowFrames * 1e9 / double(nowNs - windowStartNs),
                     (long long)skipped);
            windowStartNs = nowNs;
            windowFrames = 0;
        }
    }

    NDIlib_send_send_video_async_v2(sender, nullptr);  // release last async buffer
    NDIlib_send_destroy(sender);
    moo::ndi::destroy();
    MOO_LOGI("done: sent=%lld skipped=%lld audio_samples=%lld", (long long)sent,
             (long long)skipped, (long long)samplesSent);
    return 0;
}
