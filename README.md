# MooSwitcher

A live video switcher for Linux + NVIDIA: NDI inputs, program/preview switching with
transitions, NDI and SRT (HEVC/NVENC) program outputs, full audio mixer, Qt 6 GUI with
Vulkan multiview. Built for low latency at up to 8K 59.94p.

Status: **v1 complete (M0–M6), v2 frame sync landed**. 8K-hardened engine (30-min soak: zero
tick overruns, 1.5-frame latency, <2 cores full pipeline, NVENC 54%), full audio mixer (A/V
within ±8 ms on NDI and SRT paths at 1080p and 8K), live source picker (swap NDI/SRT sources
per input mid-show), show-file persistence (restart restores everything), health banners,
runtime counters in the GUI. Per-input **frame sync**: re-times a source onto the output tick
grid (1–4 frame buffer absorbs bursty delivery, rate slip becomes counted repeats/drops) and
auto-aligns the input's audio to the re-timed video — the cross-session A/V phase lottery
collapses to a constant (design + measurements: `docs/design-framesync.md`,
`docs/bench-framesync.md`). Bench record: `docs/bench-m5.md`; tuning: `docs/tuning.md`.
SpeedHQ measured (plan risk #1 closed): same-host NDI is compressed too; 8K NDI ingest is
not viable (use SRT/HEVC — NVDEC), NDI is comfortable through 4K; see `docs/bench-m5.md`. Milestones: M6 (v1 close), M5 (8K hardening), M4 (audio),
M3+M3.5 (SRT/HEVC both directions), M2 (switching/multiview), M1 (Vulkan engine), M0 (bench).

```sh
# SRT out (listener) + receive with any ffplay/OBS caller:
./build/mooswitcher --input CamA --input CamB --srt-out "srt://:9710?mode=listener&latency=120000"
ffplay "srt://HOST:9710?mode=caller"     # latency option is MICROseconds
# ffplay buffers seconds on live streams; for a low-latency audio monitor use:
ffmpeg -fflags nobuffer -flags low_delay -i "srt://HOST:9710?mode=caller" -vn -f pulse Mon
# SRT ingest as an input (audio decodes too; trim its lateness per input):
./build/mooswitcher --srt-input "srt://HOST:9710?mode=caller&latency=120000" --input CamB
# A/V sync check on a TS capture (needs testgen content on program):
ffmpeg -y -copyts -i "srt://HOST:9710?mode=caller" -c copy -avoid_negative_ts disabled \
       -muxpreload 0 -muxdelay 0 -t 20 cap.ts && python scripts/av_offset_ts.py cap.ts
```

Run it:
```sh
./build/moo-testgen --name CamA &  ./build/moo-testgen --name CamB &
./build/mooswitcher --input CamA --input CamB     # or moo-headless for no GUI
```

The show (inputs, outputs, transition, program/preview, full mixer state) persists to
`~/.config/MooSwitcher/show.ini` (or `--show-file PATH`) and restores on restart; CLI flags
override what they name. Click an input's name in the mixer to pick a different source live —
the browser lists NDI and OMT discovery together (Open Media Transport ingest is optional,
8K-capable for realistic content: build steps `docs/omt.md`, measurements `docs/bench-omt.md`;
headless `--omt-input`), and the manual field takes an `srt://` or `omt://` URL or an NDI
name substring. The same dialog sets the input's frame sync (Off / Trim only /
1–4 frames; headless: `--framesync IDX[:FRAMES]`). Use 1 frame for free-running cameras
you switch between often (constant A/V at +1 frame latency); Trim only suits audio-early
sources like SRT loopbacks. Shortcuts: `Space` cut, `Enter` auto, `F` FTB, `1–9` program,
`Shift+1–9` preview, `D`/`Shift+D` DSK 1/2.

Two downstream keyers composite graphics with **native alpha (NDI/OMT UYVA)** over program —
point a DSK at an input carrying alpha (CasparCG, OBS with alpha, `moo-testgen --uyva`), and
toggle it on; it fades over its own duration, independent of transitions, and FTB takes it
out with everything else. A source without alpha keys fully opaque (a fadeable fullscreen
overlay). Headless: `--dsk K:SRC --dsk-fade K:TICKS --dsk-toggle-after S:K`. Design:
`docs/design-dsk.md`; measurements: `docs/bench-dsk.md` (an 8K UYVA key over an 8K program
holds full rate; keyers-off cost is nil).

## Build

Requires: gcc 14+/clang, CMake 3.25+, Ninja, and the NDI SDK 6.

```sh
# NDI SDK (unprivileged): download + extract into third_party/ndi
# (or install AUR 'ndi-sdk' system-wide, or set NDI_SDK_DIR)
curl -L -o /tmp/ndi.tar.gz https://downloads.ndi.tv/SDK/NDI_SDK_Linux/Install_NDI_SDK_v6_Linux.tar.gz
tar xzf /tmp/ndi.tar.gz -C /tmp && (cd third_party && echo y | sh /tmp/Install_NDI_SDK_v6_Linux.sh >/dev/null && mv "NDI SDK for Linux" ndi)

cmake -B build -G Ninja
cmake --build build
ctest --test-dir build          # unit tests
```

NDI discovery needs `avahi-daemon` running.

## Tools

```sh
# Test pattern sender: counter/timestamp strips, flash+tone A/V sync burst
./build/moo-testgen --size 7680x4320 --fps 60000/1001 --name Cam1

# Latency analyzer: fps, frame gaps, end-to-end latency, A/V offset
./build/moo-latmeter --source Cam1 --csv out.csv

# SpeedHQ/transport bench (same-host measures NDI's shared-memory path;
# run the pair across two machines to measure the actual codec)
./scripts/bench_speedhq.sh 7680x4320 30 build
```

## Layout

- `src/core/` — MediaClock (rational fps, drift-free), SPSC ring / latest-frame mailbox, stats
- `src/engine/` — SwitcherCore: pure program/preview + transition state machine (unit-tested)
- `src/ndi/` — SDK lifecycle wrapper (all NDI includes route through `NdiLib.h`)
- `tools/` — moo-testgen / moo-latmeter + shared pattern layout (`tools/common/pattern.h`)
- `docs/` — bench reports; the full v1 plan lives with the project owner

---
NDI® is a registered trademark of Vizrt NDI AB. The standard NDI SDK is royalty-free
including commercial use, but commercial applications must be registered with
licensing@ndi.video before distribution; review the NDI SDK EULA before shipping binaries.
