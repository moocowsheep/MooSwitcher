# MooSwitcher

A live video switcher for Linux + NVIDIA: NDI inputs, program/preview switching with
transitions, NDI and SRT (HEVC/NVENC) program outputs, full audio mixer, Qt 6 GUI with
Vulkan multiview. Built for low latency at up to 8K 59.94p.

Status: **M0 complete** — core engine primitives (rational media clock, SPSC rings,
switcher state machine), NDI test instrumentation (`moo-testgen`, `moo-latmeter`), and the
8K throughput bench. See `docs/bench-m0.md`. Next: M1 (Vulkan engine + Qt multiview + cut).

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
