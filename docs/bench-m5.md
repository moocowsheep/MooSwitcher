# M5 bench record — 2026-07-06

Host: Ryzen 9 9900X (12C/24T), 62 GB DDR5, RTX 5090 (PCIe **Gen5 x8**
confirmed under load — lane-shared with the 10 GbE NIC), driver 610.43,
CachyOS. All video 59.94p, audio 48 kHz stereo. Same-host NDI =
uncompressed shared memory (SpeedHQ not engaged — see "open item").

## Topology validation (60 s runs)

| topology | result |
|---|---|
| 2×8K NDI in → 8K show, no outputs | ticks 3597, **skips 0**, drops 0, aud sk 0 |
| 1×8K in → 8K show → NDI out + latmeter | **lat 26.8–29.3 ms (1.6–1.76 fr ≤ 2.5)**, gaps 0, skips 0 |
| 1×8K in → NDI out + SRT out + latmeter + SRT sink | clean; profiling below |
| 2×8K in + 8K NDI out + 8K local receiver | **overload** (stale frames, gaps): ~5×4 GB/s same-host streams saturate memory bandwidth. Documented topology limit; 2×8K production sources must come from the network |

## A/V sync (flash+tone; gate ±10 ms; master delay 0, 5 ms mixer chunks)

| path | offset |
|---|---|
| 1080p NDI (4 sequential runs, one session) | +1.32…+1.40 ms |
| 1080p NDI (different session) | −7.24 ms |
| 1080p SRT | −3.81 ms |
| 8K NDI | +1.65 ms |
| 8K SRT | +7.88 ms |

Session-to-session scatter (±8 ms) is the phase of the free-running source
clock vs the switcher tick grid — irreducible without a frame-sync delay
(v2 candidate). Within-session repeatability is ~0.1 ms.

Calibration constants: SRT video pts = tick+1 and SRT audio pts +400
samples (together ≈ the ~1.5-tick real emission lag of the frame pipeline
vs the mixer); NDI uses SDK send-time timecodes on both streams.

## CPU / GPU under full 8K pipeline

| process | CPU (of one core) |
|---|---|
| moo-headless (capture+render+mixer+outputs) | **186 %** |
| moo-testgen 8K (precomputed) | 75 % |
| moo-latmeter 8K receive | 57 % |

GPU: SM 32 %, NVENC 54 % (one 8K60 HEVC session), mem ctrl 6 %.
PCIe Gen5 x8 ≈ 32 GB/s — upload (4 GB/s) + readback (4 GB/s) + NVENC
copies fit with wide margin.

## 30-min 8K soak

Topology: 1×8K in → 8K show → NDI out (latmeter attached) + SRT/HEVC out
(caller sink attached) + audio, 30 min.

| gate | result |
|---|---|
| render tick overruns | **0** (113,888 ticks) |
| audio tick overruns / underruns | **0 / 0** (380,013 ticks) |
| latmeter latency | mean **25.2 ms (1.51 fr)**, worst window 25.8, abs max 43.1; **0 windows over the 2.5-frame gate** |
| continuity | gaps 5, repeats 5, mailboxSkips 7 (connect transients) |
| NVENC | encoded all 113,888 ticks 1:1 |
| RSS | 1.49 GB → 1.29 GB (no leak) |

### Soak finding #1 — marginal-phase judder (fixed)

The first soak drew a bad clock phase and showed 35 k repeat+skip pairs
(~19/s judder): same-host source and switcher grids are phase-locked per
run, and at a marginal phase every render tick caught the newest 66 MB
frame mid-DMA, showed the previous frame, and the skipped one was gone by
the next tick. Short validation runs had all drawn lucky phases. Fix: the
input mailbox retains the last 3 publishes and the tick takes the newest
*completed* upload (`in*.lateFallbacks` counts engagements). Verified over
12 phase draws: marginal runs absorb ~1150 fallbacks/20 s with ~0 skips.
Residual: rare multi-frame stalls of the co-located 8K sender remain
visible in the counters (~5/s in 1 of 12 draws) — inherent to the bench
topology, masked only by a real jitter buffer (out of v1 scope).

### Soak finding #2 — apparent A/V drift is NTP slew

The soak's NDI av read +9.7 → +17.2 ms over 30 min (+3.9 ppm). Video
timecodes are realtime-clock snapshots at send; the SDK extrapolates
audio timecodes from sample counts; systemd-timesyncd slews the realtime
clock — the "drift" lives in the measurement clocks, not the pipeline
(audio ring underruns/trims = 0 all soak; both send paths are locked to
the monotonic show clock). Long NDI-timestamp-driven playout chains
should discipline clocks with PTP, not NTP, if sub-frame sync matters
across hours.

## SpeedHQ codec cost (plan risk #1) — MEASURED

Bench: `scripts/ndi-netns-bench.sh` (net+mount+UTS namespaces fake a second
machine; veth >> 10 GbE so the codec, not the wire, binds) with
`moo-testgen --noise` (video-range noise = worst-case codec content; the
default bars compress ~1000× and hide the codec entirely — which is also
why every earlier "same-host is uncompressed/free" observation was wrong).

**Correction to the M0 finding: same-host NDI is ALSO SpeedHQ-compressed**
(NDI 6.3.2). The sender encodes every frame whether or not receivers exist,
and local receivers decode: local vs remote noise receive cost is within 2%
(51.7% vs 53.5% of a core at 1080p), and local latency jumps 1.6 ms →
12.1 ms for noise vs bars. "shm = uncompressed" was a bars artifact.

| measurement (59.94p, noise unless noted) | result |
|---|---|
| wire bitrate: 1080p / 8K noise / 8K mid-entropy* | 0.50 / 5.8 / 1.8 Gbps |
| encode, in-sender, always-on: 1080p / 8K | ~0.25 / **~1.8–2.0 cores** |
| network TX on top when pulled: 8K @5.8G / @1.8G | +0.4 / +0.3 cores |
| decode+receive: 1080p (local ≈ remote) | ~0.5 cores |
| decode 8K noise, remote | **~4 cores AND fails**: 2/3 frames dropped, 316 ms latency |
| receive 8K noise, same-host local channel | **hard stall: 0 frames delivered**, sender pinned at 337% (SDK envelope limit at ~12 MB/frame) |
| engine: 8K mid-entropy* program + NDI out | 175% (no consumer) / 205% (remote consumer @1.8 Gbps), 0 tick overruns |

\* mid-entropy = 1080p noise upscaled to the 8K show — a realistic
stand-in between bars and raw noise.

**Verdicts.** Full-bandwidth 8K NDI *ingest* of real-entropy content is not
viable on this CPU (decode wall ~4 cores/stream, and the 6.3.2 local channel
stalls outright at 8K-noise frame sizes) — **SRT/HEVC via NVDEC is the 8K
transport** (proven 8K60 in M3/M5). NDI ingest is comfortable through 4K
(worst-case ~2 cores, realistic ~1). Our 8K NDI program out with realistic
content costs ~2 cores total including the always-on encode and holds 59.94
with zero overruns; at worst-case 5.8 Gbps the 10 GbE link itself would
saturate — the plan's "NDI out at 4K proxy while SRT carries native 8K"
fallback is the right production shape. The M5 same-host topology-limit
finding stands empirically, but its mechanism includes SpeedHQ encode/decode
on every local hop, not just memcpy bandwidth.
