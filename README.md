# MooSwitcher

A live video switcher for Linux + NVIDIA: NDI inputs, program/preview switching with
transitions, NDI and SRT (HEVC/NVENC) program outputs, full audio mixer, Qt 6 GUI with
Vulkan multiview. Built for low latency at up to 8K 59.94p.

Status: **M2 complete** — a real switcher: mix + 5 wipes + FTB with AUTO and a T-bar, the
**NDI program output** (GPU UYVY pack → SDK encodes from our readback ring), tally to sources
(and back — testgen displays it), and a proper multiview (proxy-downscaled tiles, labels from
an embedded font, tally borders). Measured end-to-end latency through the full chain
(NDI in → composite → NDI out): **17.7 ms ≈ 1 frame** at 1080p59.94. Earlier: M1 (Vulkan
engine, 2×8K zero-drop ingest), M0 (instrumentation + 8K bench, `docs/bench-m0.md`).
Next: M3 (SRT output via NVENC/HEVC).

Run it:
```sh
./build/moo-testgen --name CamA &  ./build/moo-testgen --name CamB &
./build/mooswitcher --input CamA --input CamB     # or moo-headless for no GUI
```

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
NDI® is a registered trademark of Vizrt NDI AB.
