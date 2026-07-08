# Frame-sync bench record (v2, 2026-07-07)

Design: `docs/design-framesync.md`. Box: RTX 5090 / 9900X (see `docs/bench-m5.md`
for the machine record). All runs same-host: 2× `moo-testgen` 1080p59.94 (flash+tone)
→ `moo-headless` (NDI out + SRT out listener :9710) → `moo-latmeter` (NDI-path av)
+ `ffmpeg -copyts` TS capture → `scripts/av_offset_ts.py` (SRT-path av, the
content-level ground truth — sender-timecode-free, pts-exact).

Each row of `scatter.csv` is one full engine restart = one independent phase draw.
Modes: `off` = v1 latest-frame policy; `m0` = frame sync measure-only (N=0);
`m1` = frame sync N=1; `m1b` = N=1 after `kSyncTrimBiasNs = +1.7 ms` landed.
Harness: scratchpad `fsbench/scatter.fish` (SRT capture on :9720; the m1b
series starts its capture at t=20 s — see the convergence note below).

## Results — A/V offset across independent phase draws

SRT-path av (TS capture ground truth, ms; positive = audio late). "Settled"
rows exclude captures whose per-flash spread shows the first-lock trim slew
still walking during the window (see convergence note):

| mode        | n  | mean  | σ    | range            |
|-------------|----|-------|------|------------------|
| off (v1)    | 6  | +0.09 | 6.81 | [−8.80, +7.88]   |
| m0 (N=0)    | 6  | +0.55 | 2.97 | [−3.19, +3.38]   |
| m1 (N=1), all draws | 12 | −5.30 | 0.48 | [−5.63, −3.86] |
| m1 settled only     | 8  | −5.54 | 0.06 | [−5.63, −5.46] |
| m1b settled (the verify set) | 6 | −3.90 | **0.01** | [−3.92, −3.89] |

Latmeter NDI-path av, same draws: off σ 9.82 / m0 σ 6.71 / m1 σ 5.4–5.8 — the
residual m1 scatter there is the measurement method (see Reading), not the mix.

**Gate: σ ≤ 1 ms across sessions — PASS at N=1 (settled σ 0.01 ms over the
6-draw verify set, vs v1's 6.81 ms; v1's full range was a frame period
wide).** Per-draw trims ranged 4.7–16.1 ms over the verify set while av
stayed within ±0.02 ms — the trim is absorbing real per-session phase/fill
differences, exactly the design intent. The +1.7 ms bias centers the N=1
draws on −3.90, i.e. the v1 SRT-path calibration center (−3.8).

videoDelay gauge ranged 17.6–31.5 ms over the verify draws (N·T + tick-phase
remainder, as designed: the [T, 2T) window at N=1).

### First-lock convergence transient (found by the m1b draws)

Early m1b draws produced apparent outliers (−2.20, +1.36) with per-flash
ramps of ~1.04 ms per flash — exactly the trim slew rate (1 ms/s). Cause:
the instant first-lock (Q2) applies the EWMA estimate as of lock time, ~1–2 s
after connect, while the estimators still carry startup-transient bias; the
residual (up to ~11 ms observed) then decays at the live slew rate, so a
session can take up to ~15 s after input connect to fully center. Every ramp
capture's final flash lands on the settled cluster (−3.87/−3.89 vs −3.90;
the one m1 ramp draw −5.57 vs −5.54), confirming all sessions converge to
the same value. Operator-visible consequence: give a freshly connected input
~15 s before judging its A/V trim; the scatter harness now starts its
capture at t=20 s. (Two orphaned early draws were discarded outright: they
ran while two engine instances fought over one SRT listener port, so their
captures cannot be attributed — visible as reconnect storms in the logs.)

## Reading

- **The P1 gate is the SRT-path (TS ground truth) column.** The latmeter's av
  number pairs sender timecodes stamped by our NDI output at send time; its
  per-session quantization noise (mixer-grid vs tick-grid stamping phase) is
  several ms and dominates its scatter. The TS capture measures content
  against content with exact pts and is the number the ±10 ms A/V gate is
  defined on.
- **m0 (measure-only) does not fix NDI-input scatter on this box**: the audio
  lane's natural latency exceeds N=0 video presentation delay, the required
  trim is negative, and it clamps at 0 (visible as `inN.sync.trimClamped`).
  Use N≥1 for NDI inputs; m0 works for audio-early inputs — SRT-in measures
  audio-early once the connect-backlog parking fix is in (see the SRT-input
  section; the old "~44 ms early" folklore was one draw of that bug).
- **Trim variance across draws is by design**: the trim absorbs the video
  tick-phase lottery AND the audio ring-fill lottery, so it differs per
  session while the resulting A/V offset stays put.
- NDI audio input pts are read as first-sample times (`af.timestamp`), NOT
  corrected by chunk length — the correction was tried and overshoots a full
  chunk against the TS ground truth.
- `kSyncTrimBiasNs = +1.7 ms` centers the N=1 NDI-in draws on the v1 SRT-path
  calibration center (−3.8 ms).

## Latency (single draws, latmeter lat_avg)

- off: 13.1 ms · m0: 17.3 ms · m1: 35.9 ms — consistent with +1 frame for N=1
  modulo the per-draw phase term; N=0 within the phase-noise band of off.

## SRT-input path check

Chain: testgen → headless#1 (SRT out) → headless#2 (`--srt-input`, sync cfg
under test, SRT out) → `-copyts` capture of headless#2's output (content
ground truth through the full double hop).
Real TS pts used end-to-end (ptsSynth 0, resyncs 0), NVDEC connect burst
absorbed after the documented startup transient (overflows ≤ 3, drops at
acquire only in the first second), steady depth 1.

### The stale 44 ms, and the connect-backlog parking bug (fixed)

The design predicted the SRT loopback auto-trim would reproduce v1's
hand-set `--input-delay 0:44`. Measured content-level instead (pre-fix):

- sync off, bare: **+17.2 ms audio-late**, wandering ±16.7 ms in exact
  frame-period steps (P2's quantized video slips; spread 33 ms over 14 s).
- sync off + the v1 44 ms trim: **+35.9 ms audio-late** — the hand-set
  value now *adds* misalignment. It had calibrated one random draw of the
  bug below and is obsolete.
- The trim demand across identical N=1 sessions ranged **−21.4 ms (clamps
  at 0 — audio cannot be advanced) to +31.7 ms (applied)**.

Cause: a connect/reconnect lands the transport's buffered backlog (SRT
latency window, 100+ ms) in the audio ring within one mixer tick; the
prefill hold releases at fill ≥ prefill, parking a random
[prefill, kMaxFill] of extra audio latency for the session. Video drops
the same backlog (sync overflow/resync at acquire); audio kept it. Fix:
sync-managed lanes trim the ring to exactly prefill on every hold release
(`InputChannel::syncManaged`; inaudible — the lane was holding). Off-mode
is untouched (G4).

### Post-fix results

- Trim demand deterministic and positive: N=0 applies +5.5 ms, N=1
  +8.6 ms, clamp residual ≤ 0.13 ms. **N=0 now works for SRT-in** (the
  audio-early case the design reserved it for).
- Content-level N=1, two independent sessions: baseline av **−0.08 ms in
  both** — per-session DC repeatability at the same level as the NDI verify
  set, and the ±10 ms gate holds with **no hand-set trim**. N=2 session:
  same class (+0.76 avg incl. one excursion, videoDelay 34.7 ms).
- Within-session residual: occasional single-frame (±16.7 ms) av
  excursions under co-located bench load — NVDEC burst gaps exceeding the
  sync window (starve-repeat / slip-catch-up, `sync.starves`/`slipDrops`
  count them). Each recovers by the next flash; the trim deadband rides
  through instead of chasing. Use deeper N for burstier feeds; this is the
  documented "not a jitter buffer" limit.
- NDI regression after the fill-trim fix: two fresh m1b draws land at
  −3.91/−3.88 (spread 0.00) — the verify-set center is unmoved. Per-draw
  trims grew to ~30 ms (the lane no longer parks arrival clumps, so the
  trim absorbs them instead) with an identical resulting offset: the trim
  doing its job. Unit test: "sync-managed lane drops connect backlog at
  hold release" (56th test).

## Soak (test 5) — PASSED 2026-07-07

30 min, 1080p NDI + 8K-bars-over-SRT (NVENC→NVDEC), both N=1, autos every
2.5 s, NDI out. 107,895 render ticks.

- **0 tick skips**; NDI out delivered every tick. Both inputs held full
  59.94 (in0 107,866 / in1 107,869 frames).
- **Audio: 0 skips; underruns = 1**, in the first second during SRT
  connect (lane arm), frozen for the remaining 29:59. Steady state meets
  the 0-underrun gate; the arm-time blip is the trim-to-prefill fix
  leaving no headroom when the first post-arm chunk lands late —
  inaudible (lane re-arms silently), noted as expected.
- **RSS flat**: 60 samples, 4.3 MB band around 405 MB.
- **sync.starves: in0 (NDI) = 0** — the gate as written, met exactly; in0
  ran the entire soak with *no* sync events at all. in1 (8K SRT/NVDEC,
  co-located sender) logged the characterized burst residual: 11 starves /
  43 slipDrops / 4 overflows / 27 waits over 107,895 ticks (~1 single-frame
  excursion per 100 s; see the SRT-input section — this is the documented
  N=1 limit under co-located load, not a regression).
- Auto trim finals: in0 31.3 ms, in1 28.1 ms applied; in1 trimClamped
  gauge 0.2 ms (deadband noise). The change-detecting log monitor saw zero
  counter movement between the first status line and shutdown.

Frame sync validation (tests 1–5) is complete; the listening test
(test 6) remains a human item.
