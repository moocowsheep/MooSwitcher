# MooSwitcher

A live video switcher for Linux + NVIDIA: NDI inputs, program/preview switching with
transitions, NDI and SRT (HEVC/NVENC) program outputs, full audio mixer, Qt 6 GUI with
Vulkan multiview. Built for low latency at up to 8K 59.94p.

Status: **v1 complete (M0–M6)**. 8K-hardened engine (30-min soak: zero tick overruns,
1.5-frame latency, <2 cores full pipeline, NVENC 54%), full audio mixer (A/V within ±8 ms
on NDI and SRT paths at 1080p and 8K), live source picker (swap NDI/SRT sources per input
mid-show), show-file persistence (restart restores everything), health banners, runtime
counters in the GUI. Bench record: `docs/bench-m5.md`; tuning: `docs/tuning.md`.
Open item: SpeedHQ codec cost needs a remote peer (`scripts/ndi-netns-bench.sh`, one sudo
run, or a second box). Milestones: M6 (v1 close), M5 (8K hardening), M4 (audio),
M3+M3.5 (SRT/HEVC both directions), M2 (switching/multiview), M1 (Vulkan engine), M0 (bench).

```sh
# SRT out (listener) + receive with any ffplay/OBS caller:
./build/mooswitcher --input CamA --input CamB --srt-out "srt://:9710?mode=listener&latency=120000"
ffplay "srt://HOST:9710?mode=caller"     # latency option is MICROseconds
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
override what they name. Click an input's name in the mixer to pick a different NDI source
or an `srt://` URL live. Shortcuts: `Space` cut, `Enter` auto, `F` FTB, `1–9` program,
`Shift+1–9` preview.

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
