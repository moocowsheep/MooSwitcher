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

## Open item — SpeedHQ codec cost (plan risk #1)

Still unmeasured: same-host shm bypasses the codec and ignores transport
config (verified: TCP-only ndi-config leaves CPU and 1.6 ms shm latency
unchanged). Requires either the second box on the 10 GbE link (was
offline during M5) or one sudo run of `scripts/ndi-netns-bench.sh`.
Budget until then: 2–4 cores per remote 8K decode.
