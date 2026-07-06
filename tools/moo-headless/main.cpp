// moo-headless: runs the switcher engine without a GUI. Verification driver
// for M1: connects NDI inputs, renders the multiview, dumps PPM frames,
// exercises cut via a scripted schedule, prints stats.

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/ppm.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"
#include "engine/Engine.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    moo::EngineConfig cfg;
    double duration = 10;
    std::string dumpDir;
    double dumpEvery = 2.0;
    bool scriptedCuts = false;
    bool scriptedAutos = false;
    std::vector<std::pair<int, int>> inputDelays;  // {input, ms} audio trims

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--input") {
            if (const char* v = next())
                cfg.inputs.push_back({moo::InputSpec::Type::Ndi, v});
        } else if (a == "--srt-input") {
            if (const char* v = next())
                cfg.inputs.push_back({moo::InputSpec::Type::Srt, v});
        } else if (a == "--show") {
            const char* v = next();
            if (!v || sscanf(v, "%dx%d", &cfg.show.width, &cfg.show.height) != 2)
                return 2;
        } else if (a == "--duration") {
            const char* v = next();
            if (v) duration = atof(v);
        } else if (a == "--dump-dir") {
            if (const char* v = next()) dumpDir = v;
        } else if (a == "--dump-every") {
            const char* v = next();
            if (v) dumpEvery = atof(v);
        } else if (a == "--cuts") {
            scriptedCuts = true;  // cut every 2 seconds
        } else if (a == "--autos") {
            scriptedAutos = true;  // auto-transition every 2.5s, cycling types
        } else if (a == "--no-ndi-out") {
            cfg.ndiOut = false;
        } else if (a == "--srt-out") {
            if (const char* v = next()) cfg.srtUrl = v;
        } else if (a == "--srt-bitrate") {
            const char* v = next();
            if (v) cfg.srtBitrateKbps = atoi(v);
        } else if (a == "--no-audio") {
            cfg.audio = false;
        } else if (a == "--audio-delay") {
            const char* v = next();
            if (v) cfg.masterAudioDelayMs = atoi(v);
        } else if (a == "--input-delay") {
            const char* v = next();
            int idx = 0, ms = 0;
            if (!v || sscanf(v, "%d:%d", &idx, &ms) != 2) return 2;
            inputDelays.emplace_back(idx, ms);
        } else if (a == "--validate") {
            cfg.validation = true;
        } else {
            fprintf(stderr,
                    "usage: moo-headless --input NAME [--input NAME ...] "
                    "[--show WxH] [--duration S] [--dump-dir D] "
                    "[--dump-every S] [--cuts] [--no-audio] "
                    "[--audio-delay MS] [--validate]\n");
            return 2;
        }
    }
    if (cfg.inputs.empty())
        cfg.inputs = {{moo::InputSpec::Type::Ndi, "MooBenchA"},
                      {moo::InputSpec::Type::Ndi, "MooBenchB"}};
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    moo::Engine engine;
    if (!engine.start(cfg)) {
        MOO_LOGE("engine start failed");
        return 1;
    }
    if (auto* aud = engine.audio())
        for (auto [idx, ms] : inputDelays)
            if (idx >= 0 && idx < aud->inputCount())
                aud->channel(idx).delayMs.store(ms);

    const int64_t t0 = moo::MediaClock::nowNs();
    const int64_t endNs = t0 + int64_t(duration * 1e9);
    int64_t nextDumpNs = t0 + int64_t(dumpEvery * 1e9);
    int64_t nextCutNs = t0 + 2'000'000'000;
    int64_t nextLogNs = t0 + 1'000'000'000;
    int dumpIdx = 0;
    int transCycle = 1;  // start with WipeLR

    std::vector<uint8_t> mv;
    uint64_t mvSeq = 0;
    int mvW = 0, mvH = 0;

    while (!g_stop && moo::MediaClock::nowNs() < endNs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t now = moo::MediaClock::nowNs();

        if (scriptedCuts && now >= nextCutNs) {
            engine.post({moo::Command::Type::Cut, 0, 0, 0.f});
            nextCutNs += 2'000'000'000;
        }
        if (scriptedAutos && now >= nextCutNs) {
            // Cycle transition types (mix + all wipes), 45-frame duration.
            engine.post({moo::Command::Type::SetTransition, transCycle % 7, 45, 0.05f});
            engine.post({moo::Command::Type::Auto, 0, 0, 0.f});
            ++transCycle;
            nextCutNs += 2'500'000'000;
        }
        if (!dumpDir.empty() && now >= nextDumpNs) {
            if (engine.copyMultiview(mv, mvSeq, mvW, mvH)) {
                char path[512];
                snprintf(path, sizeof path, "%s/mv_%03d.ppm", dumpDir.c_str(),
                         dumpIdx++);
                moo::ppm::writeRgba(path, mvW, mvH, mv.data());
                MOO_LOGI("dumped %s (seq %llu)", path, (unsigned long long)mvSeq);
            }
            nextDumpNs += int64_t(dumpEvery * 1e9);
        }
        if (now >= nextLogNs) {
            std::string line = "ticks=" + std::to_string(engine.renderedTicks()) +
                               " skips=" + std::to_string(engine.skippedTicks());
            if (auto* aud = engine.audio())
                line += "  aud[t=" + std::to_string(aud->mixTicks()) +
                        " sk=" + std::to_string(aud->mixSkips()) +
                        " un=" + std::to_string(aud->underruns()) + "]";
            if (engine.srtFramesEncoded() || engine.srtConnected())
                line += "  srt[" + std::string(engine.srtConnected() ? "up" : "down") +
                        " enc=" + std::to_string(engine.srtFramesEncoded()) + "]";
            for (int i = 0; i < engine.inputCount(); ++i) {
                const auto s = engine.inputStatus(i);
                line += "  in" + std::to_string(i) + "[" +
                        (s.connected ? "up" : "down") + " " +
                        std::to_string(s.desc.width) + "x" +
                        std::to_string(s.desc.height) +
                        " f=" + std::to_string(s.frames) +
                        " d=" + std::to_string(s.drops) + "]";
            }
            MOO_LOGI("%s", line.c_str());
            nextLogNs += 1'000'000'000;
        }
    }

    engine.stop();

    MOO_LOGI("-- final counters (nonzero) --");
    for (const auto& s : moo::Stats::snapshot())
        if (s.value)
            MOO_LOGI("  %-28s %lld", s.name.c_str(), (long long)s.value);
    return 0;
}
