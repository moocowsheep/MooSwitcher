# OMT input bench (2026-07-07)

Setup: `moo-testgen --omt` → `moo-headless --omt-input` (discovery name),
same host (RTX 5090 / 9900X, see `docs/bench-m5.md`), sender quality
Default, 45–50 s runs, CPU sampled over 15 s of steady state with
`scripts/cpu_sample.py` (100% = one core; testgen number includes pattern
stamping). Content per the SpeedHQ lesson: bars = friendly case, `--noise`
= worst case; real content sits between. Harness: session scratchpad
`omt-bench.fish`.

## 1080p59.94

| content | enc CPU | dec+ingest CPU | wire | enc ms/f | drops | rate |
|---------|---------|-----------------|------|----------|-------|------|
| bars    | 0.08c   | 0.07c           | 14.8 Mbps | 0.71 | 0 | 59.94 full |
| noise   | 0.48c   | 0.44c           | 585 Mbps  | 4.2  | 0 | 59.94 full |

Same class as NDI/SpeedHQ on this box (noise: ~0.25c enc / ~0.5c dec /
0.5 Gbps). Engine ingest clean both runs: `skips=0`, `d=0`, audio 0/0.

## 8K (7680×4320@59.94)

| content | enc CPU | dec+ingest CPU | wire | enc ms/f | envelope drops | received rate |
|---------|---------|-----------------|------|----------|----------------|---------------|
| bars    | 2.5c    | 3.1c            | 148 Mbps  | 3.9 | 0 | 59.94 full |
| mid (8×8 blocks) | 2.9c | 3.4c       | 1.93 Gbps | 4.7 | 0 | 59.94 full, d=6/2691 |
| noise   | 7.6c    | (nothing arrives) | 0     | —   | **100% (2593/2593)** | 0 |

- **VERDICT: OMT 8K works for realistic content on this box** — full 59.94
  end-to-end at bars and mid-entropy, ~1 GB RSS per side, receiver decode ~3
  cores. This is the first same-host-viable 8K alternative to SRT/NVDEC:
  NDI/SpeedHQ hard-stalls the same-host channel at 8K (SDK envelope, see
  the M0-correction bench) while OMT runs it clean. Budget ~3 cores per 8K
  OMT input (2×8K in ≈ 6 of 24 threads) vs near-free NVDEC — SRT/HEVC
  remains the preferred 8K delivery shape; OMT is now a workable second.
- Mid-entropy compresses to ~4 MB/frame (1.93 Gbps) — almost exactly
  SpeedHQ's mid-entropy wire rate (1.8 Gbps), well inside the envelope.
  `--mid` (random flat 8×8 blocks, temporal chaos) was added to testgen for
  this; bars alone are meaningless for codec benches (the SpeedHQ lesson).
- Receiver-side `mailboxSkips`/`repeats`/`lateFallbacks` at 8K bars (~4% of
  ticks) are the known same-host marginal-phase behavior on 66 MB uploads
  (M5); frame sync absorbs them when enabled.
- 8K noise: every VMX frame ≈ 19 MB > the 10 MB envelope → the sender
  counts-and-drops **all** video (audio keeps flowing; receiver stays up,
  input shows down 0x0). Encode still burned 7.6 cores producing dropped
  frames — worst case to avoid, not a crash.

## Frame sync on OMT

1080p N=1 loopback: real sender pts accepted (ptsSynth 0), auto trim
+24.4 ms applied, no clamp, videoDelay 26.1 ms (the [T,2T) window),
zero starves/slips/resyncs. The OMT audio pts lane behaves like NDI's.

## Transport envelope

`OMTConstants.VIDEO_MAX_SIZE = 10 MB` per compressed frame; the sender
drops (and counts, `omt[dropped=]`) oversize frames rather than stalling —
strictly better failure semantics than NDI's same-host 8K hard-stall at
~12 MB envelopes. Patchable in the vendored build (`third_party/libomtnet`,
MIT) if incompressible 8K content ever matters; measured mid-entropy sits
2.5× under the limit, so real content has headroom.
