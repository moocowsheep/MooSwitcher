# Frame Sync — per-input re-timing + automatic A/V trim (v2 design)

Status: IMPLEMENTED (F1–F4, 2026-07-07) — §10 records where the implementation
deviated from the plan and what the benches measured.
Prereq reading: `docs/bench-m5.md` (phase judder, A/V scatter numbers), plan §"Threading".

## 1. Problem

Two defects were measured in v1 and explicitly deferred to a frame synchronizer:

**P1 — ±8 ms cross-session A/V phase scatter.** The render tick samples each input's
mailbox once per frame period T (16.683 ms at 59.94). A source frame arriving φ before
the next tick waits φ before it can be composited, φ ∈ [0, T). Same-host clocks are
phase-locked *per run* (M5 finding), so φ is a per-session lottery: constant within a
session, uniform across sessions. Audio does not ride the video tick — it flows
capture ring → 5 ms mixer grid with a prefill-governed fill — so its latency is nearly
phase-independent. Net: the A/V offset moves by up to a video frame between sessions
(measured ±8 ms scatter around the per-path centers; the ±10 ms gate only holds
per-calibration, not across restarts). Irreducible in v1 because nothing measures φ.

**P2 — SRT-input burst gaps.** NVDEC emission is bursty (mux stalls, co-located sender
stalls). The latest-frame mailbox is *correct switcher policy* but converts a burst of
k frames into k−1 `mailboxSkips` and a repeat on the starved side — counted, accepted
in v1. A small queue drained on the tick grid would present those frames instead.

Related but already mitigated: marginal-phase judder (M5's `LatestMailbox` kKeep=3
retention + newest-COMPLETED pick). Frame sync supersedes that fix *when enabled*; the
retention stays for sync-off inputs.

## 2. Goals / non-goals

Goals
- G1: per-input opt-in frame synchronizer that re-times video onto the render tick
  grid with a deterministic target delay of N source frames (N = 0..4).
- G2: continuous measurement of each input's *realized* video delay (φ included) and
  of its audio lane's realized delay, in a shared per-input timestamp domain; the
  difference drives an automatic per-input audio trim. This is what kills P1 — the
  video buffer alone cannot (φ quantization is inherent to a tick-based switcher).
- G3: burst absorption up to the configured depth (P2), with deterministic,
  counted repeat/drop behavior under rate slip (60.000 source on a 59.94 show, ppm
  drift) instead of the phase lottery.
- G4: zero behavior change when off. Off remains the default; the v1 latency target
  (≤2.5 frames NDI→NDI) is quoted with sync off.

Non-goals
- Full jitter buffer for lossy-network SRT (SRT's own latency window handles
  retransmit; we only absorb *decoder emission* burstiness).
- Cross-input genlock or timecode-based multi-camera alignment (needs PTP story, v3+).
- Audio resampling. Trim is a delay tap, as today; non-48k sources stay out of scope.
- Changing the master calibration constants (SRT pts=tick+1, masterDelayMs, etc.).
  Auto-trim is relative per input; path-level centering stays where M4/M5 put it.

## 3. Where it sits

A new pure class — `src/engine/FrameSync.{h,cpp}` — owned per input by the Engine
render loop, same testability contract as `SwitcherCore` (no threads, no clocks, no
I/O; everything simulated in unit tests). The capture/decode threads do NOT schedule;
they only gain timestamping and a deeper feed path.

```
capture thread                      render thread (per tick n, time t_n)
  NdiReceiver / SrtInput              drain feedRing → FrameSync::push(...)
    memcpy/cuMemcpy + submit          FrameSync::present(t_n) → FramePtr | repeat
    publish(mailbox)      [sync off]  (existing newerCandidates path unchanged)
    push(feedRing)        [sync on]
                                    audio thread
  audio push → InputChannel ring      mixer applies delayFrames =
    + pts bookkeeping at push           manual trim + autoTrim (slewed)
```

### 3.1 Timestamps at publish

`GpuFrame` gains no fields (it is a GPU-lifetime object); instead the sync feed
carries them:

```cpp
struct TimedFrame {                 // src/engine/FrameSync.h
    IInputSource::FramePtr frame;
    uint64_t seq;                   // mailbox seq domain, shared with sync-off path
    int64_t  srcPtsNs;              // sender/pts clock, see 3.2
    int64_t  arrNs;                 // MediaClock::nowNs() at submit()
};
```

- `NdiReceiver::run`: `srcPtsNs = vf.timestamp * 100` (SDK 100 ns units, sender
  realtime at send). **Trust deltas only, never absolutes** — sender realtime is
  NTP-slewed (M5: 3.9 ppm) and unrelated to our monotonic clock. Sender-timestamp
  sanity check: if successive deltas deviate >25% from `desc` cadence for >8 frames
  (some senders emit 0 or garbage), fall back to synthesized pts = firstArr + k·Ts and
  set a per-input `sync.ptsSynth` flag/counter.
- `SrtInput::handleFrame`: `srcPtsNs = av_rescale_q(f->best_effort_timestamp,
  stream->time_base, {1, 1'000'000'000})`. Audio decode uses the same stream clock —
  that shared domain is what makes G2 exact for SRT.
- NDI audio: `af.timestamp` is the same sender realtime domain as video — shared
  domain also holds.

### 3.2 The anchor (pts → local monotonic)

Per input, sync maintains `anchor ≈ min over sliding window W of (arrNs − srcPtsNs)`
(W = 128 frames, implemented as monotonic wedge / two-level min). The min filter is
the classic dejitter estimator: bursts and queueing only ever make `arr − pts`
*larger*, so the window min tracks the true (least-delayed) path offset. Drift
handling: apply anchor corrections with a slew clamp (≤0.1 ms/tick) so a slowly
rising/falling min (ppm rate mismatch, sender NTP slew) never steps the schedule;
resync events (below) re-anchor hard.

The anchor is shared by the input's audio-side measurement (G2), so sender-clock slew
cancels out of the A/V difference entirely.

### 3.3 Scheduling

Config: `syncFrames N` (0..4). Target delay Δ = N · Ts (Ts = source frame period from
`desc`, revalidated against pts deltas). Due time: `due(f) = srcPtsNs + anchor + Δ`.

Per render tick at t_n, after draining the feed ring into an internal deque (ordered,
seq-checked):

1. Candidates = frames with `due ≤ t_n` AND `frame->uploaded()`. A due-but-still-in-
   flight head is left for next tick (counted `sync.lateUploads`; the Δ ≥ 1 case makes
   this rare by construction — the DMA has had a full frame period).
2. Present the **newest** candidate; drop older candidates (count `sync.slipDrops` —
   this is the deterministic 60.000-on-59.94 slip, one drop per ~16.7 s at 1.001×,
   and the burst catch-up path).
3. No candidate: repeat previous (count `sync.starves` if the deque is empty,
   `sync.waits` if a frame exists but isn't due/uploaded — the latter is healthy).
4. Deque overflow (> N + 4): drop oldest, count `sync.overflows`.

Resync (flush deque to newest, hard re-anchor, count `sync.resyncs`): seq gap > 32,
pts gap > 500 ms, reconnect edge, format change (Ts change), N changed by operator.

The existing stale→placeholder logic (2 s) operates on presented frames, unchanged.
N = 0 ("measure-only") schedules with Δ = 0: presentation behavior is effectively
v1's (first tick after upload), but the measurement machinery (3.4) still runs — this
is the zero-added-latency mode that still fixes P1.

### 3.4 Realized-delay measurement → auto audio trim (the P1 kill)

- Video side: on every presented frame, `dVid = t_n − (srcPtsNs + anchor)`, EWMA'd
  (α ≈ 1/64). This captures Δ + φ + upload margin — the *actual* per-session number.
- Audio side: `InputChannel::pushPlanar/pushInterleaved` gain an optional pts
  argument (same domain). At push, `dAud = (playoutEstimate − (ptsNs + anchor))`
  where `playoutEstimate = nowNs + ringFill/48k + fixed mixer pipeline latency`
  (chunk quantization ≤5 ms is phase-stable per session, same argument as video φ).
  EWMA'd identically.
- `autoTrimFrames = clamp(round((dVid − dAud) · 48k), 0, framesForMs(500) − manual)`.
  Applied in `AudioEngine`'s per-chunk params as `delayFrames = manualTrim +
  autoTrim` feeding the existing sample-granular `MixerCore::DelayLine`.
  Negative result (audio later than video — shouldn't happen with N ≥ 1 headroom)
  clamps to 0 and sets `sync.trimClamped`; the operator sees it in the GUI.
- Slew: `DelayLine` tap changes jump the read position (documented click risk). The
  auto trim therefore moves ≤ 48 frames (1 ms) per second, in single steps per chunk,
  and only when |target − current| > 24 frames (0.5 ms deadband). Big first-lock
  jump (e.g. SRT's 44 ms) happens once, within the first ~2 s of an input connecting
  — before the operator has it on air in practice. **Open question Q2** covers
  whether first-lock should be instant-while-faded instead.
- The SRT loopback case (audio ~44 ms early vs NVDEC video) is exactly `dVid − dAud ≈
  44 ms` in the shared TS pts domain → auto-trim replaces today's hand-set
  `--input-delay 0:44`. The manual spinbox remains as an *additional* operator offset.

## 4. Feed path and ring depth

- New per-input `SpscRing<TimedFrame> feedRing{16}` (producer = capture/decode
  thread, consumer = render tick), active only when sync is enabled. Publish goes to
  **both** mailbox and feedRing — the mailbox stays authoritative for sync-off and
  for instant toggling; a full feedRing drops the newest publish from the sync path
  only (count `sync.feedDrops`; capacity 16 ≫ any observed burst).
- Pinned-slot budget: a queued `TimedFrame` pins its Upload/Nv12Ring slot. Ring slot
  count becomes a constructor parameter: `kSlots(5) + syncFrames + 2` for
  sync-enabled inputs (max 11 at N=4). Cost per 8K input at N=4: +6 slots ≈ +400 MB
  pinned host + +400 MB VRAM — acceptable (32 GB VRAM / 62 GB RAM; plan's 4×8K budget
  2.4 GB VRAM), but 1080p shows pay ~25 MB. Changing N live re-uses the existing
  format-change path in the receivers (new ring, old one dies with its last published
  frame) — same mechanism, no new lifetime rules.
- `LatestMailbox::kKeep` stays 3; the M5 late-upload fallback continues to protect
  sync-off inputs.

## 5. Operator surface

- Engine config/`InputSpec`: `syncMode off|on`, `syncFrames 0..4` (0 = measure-only).
  Default off.
- Headless: `--framesync IDX[:FRAMES]` (FRAMES default 1); repeatable per input.
- GUI: per-input row in the source strip / picker dialog: checkbox "Frame sync",
  spinbox frames, read-only "auto trim: X.X ms" label next to the existing manual
  delay spinbox (MixerPanel). Health banner if `sync.resyncs` grows steadily.
- ShowFile (INI, arrays 1-based as documented): in the `inputs` array,
  `framesync = true|false`, `framesyncFrames = 0..4`; loader tolerates absence
  (defaults off) so v1 show files load unchanged.

## 6. Counters (Stats, per input, `inN.sync.*`)

`depth` (gauge), `starves`, `waits`, `slipDrops`, `overflows`, `feedDrops`,
`lateUploads`, `resyncs`, `ptsSynth`, `autoTrimMs` (gauge ×10), `trimClamped`.
All appear in the GUI stats poll and the headless exit dump like existing counters.

## 7. Validation plan (gates before merge)

1. **Unit (FrameSync pure, simulated grid)** — phase sweep 16 φ steps × traces:
   clean cadence; recorded SRT burst trace (capture one from the M5 co-located
   bench); 60.000-on-59.94 (expect exactly one slipDrop per ~16.7 s, zero starves);
   30 fps source (repeat every other tick, zero drops); 500 ms gap → resync; anchor
   slew under injected 4 ppm pts drift. Assert presented-frame sequence AND measured
   dVid accuracy (±0.1 ms) against ground truth. Also DelayLine-slew unit for the
   trim ramp. Extends the existing ctest suite (43 → ~46).
2. **A/V scatter bench (the P1 gate)** — ≥12 session phase draws (M5 methodology,
   `scripts/av_offset_ts.py`, flash+tone via testgen/latmeter): sync N=1 on 1080p NDI
   + 1080p SRT simultaneously. Gate: per-path A/V offset σ ≤ 1 ms across draws
   (v1 measured ±8 ms), centers within the existing ±10 ms gate *without* re-tuning
   manual trims. SRT input with NO hand-set 44 ms trim must self-center.
3. **Latency (G4 gate)** — latmeter NDI→NDI: sync off = v1 baseline (17.7 ms class);
   N=1 = baseline + 1 frame ± 0.2; N=0 = baseline ± 0.2.
4. **Burst (P2 gate)** — replay the co-located 8K sender-stall scenario: bursts ≤ N+2
   frames present with zero gaps (mailboxSkips-equivalent → sync.slipDrops only for
   genuine slip); bursts beyond depth still counted, documented as the limit.
5. **Soak** — 30 min, 2 inputs (1080p NDI + 8K SRT), sync N=1: M5 gates (0 overruns,
   0 audio skips/underruns, RSS flat) + `sync.starves` = 0, `autoTrimMs` stable
   (±0.5 ms) after first lock.
6. **Listening test** (human) — crossfade + a forced 40 ms auto-trim ramp audible
   check; feeds Q2.

## 8. Risks & open questions

- **R1 — NDI sender timestamp quality in the wild.** Mitigated by the cadence sanity
  check + synthesized-pts fallback (3.1); fallback still fixes P1 (arrival-based
  anchor) but weakens the audio lock to arrival-domain accuracy. Counter makes it
  visible.
- **R2 — pinned-memory growth at 4×8K + N=4** (+1.6 GB host). Documented; if it
  bites, a follow-up can hold sync-queue frames in staging-free VkImages only
  (uploads already completed by definition for queued frames — staging slot could be
  released early). Not in scope for the first cut.
- **R3 — trim ramp audibility** (DelayLine tap jumps). Deadband + 1 ms/s slew should
  be inaudible at 48 kHz speech/music; verified by test 6.
- **Q1 — default for new shows**: RESOLVED — ship off. Bench data says the
  operator guidance is per-input anyway (N≥1 for free-running cameras where
  constant A/V matters more than +1 frame; N=0 only for audio-early feeds;
  off for lowest latency). README carries the guidance.
- **Q2 — first-lock trim jump**: instant when the input is off-air/faded vs. always
  slewed. Decide after listening test.
- **Q3 — preview/multiview path**: synced everywhere (single presented frame per
  input per tick — simpler, preview shows exactly what program will show). Revisit
  only if operators want minimum-latency preview.

## 9. Milestones

- **F1** — TimedFrame plumbing (both receivers), FrameSync core + unit tests (test 1).
- **F2** — Engine integration: feed ring, parametric ring depth, present path,
  counters, `--framesync`; latency + burst benches (tests 3, 4).
- **F3** — Audio: pts at push, realized-delay measurement, auto trim + slew; scatter
  bench (test 2), listening test (test 6), resolve Q2.
- **F4** — GUI + ShowFile + docs (README, tuning.md cross-ref); soak (test 5);
  flip Q1 decision into defaults.

Estimate at v1 pace: F1 ≈ 1 day, F2 ≈ 1 day, F3 ≈ 1.5 days, F4 ≈ 0.5 day.

## 10. Implementation notes (what shipped, where it deviates)

- `FrameSync` is a header-only template (`src/engine/FrameSync.h`,
  `FrameSync<Frame>`) rather than a .h/.cpp pair: the frame handle is the
  template parameter, so the unit tests drive the scheduler with plain ints
  and zero GPU deps. `uploaded` is a predicate passed to `present()`.
- **Anchor fast-lock** (found by the burst unit test): seeding the anchor
  from the first pushed frame is wrong when the stream opens mid-burst (the
  SRT connect backlog) — it lands frames too high and the 0.1 ms/tick slew
  takes seconds to crawl down. While unlocked (< 8 presents since seed or
  resync) the applied anchor jumps straight to the window minimum; the slew
  clamp only governs the locked state.
- **Trim algebra**: the shared anchor cancels out entirely. The trim is
  computed as EWMA(present − pts_video) − EWMA(playout − pts_audio), both in
  the sender clock domain; no cross-thread anchor sharing exists. Audio-side
  measurement lives in `InputChannel` (sampled at push, before the delay
  line, so the trim never feeds back into it).
- **N=0 on NDI inputs does NOT cancel the scatter on this box** (measured,
  see `docs/bench-framesync.md`): the audio lane's natural latency exceeds
  N=0's video presentation delay, so the required trim is negative and
  clamps at 0 (`inN.sync.trimClamped` gauge shows the deficit). For NDI
  inputs, use N≥1 — the +1 frame of headroom is what makes audio delayable.
  SRT inputs measure audio-early after the connect-backlog fix below, so
  N=0 works there (bench: +5.5 ms trim applied, clamp ≈ 0).
- **Q2 resolved**: the applied trim jumps to target while the lane is silent
  (prefill hold — covers connect) or on the first lock after a generation
  bump (input replace), and slews at ~1 ms/s with a 0.5 ms deadband while
  live. `InputChannel::autoDelayGen` invalidates the mixer's applied value.
- Ring slot counts became constructor parameters (`UploadRing`/`Nv12Ring`,
  `unique_ptr<Slot[]>` — slots hold atomics and can't live in a resizable
  vector). Sync-enabled inputs allocate `kSlots + N + 2`.
- Changing an input's sync setting goes through the existing input-replace
  machinery (source picker dialog → `requestInputReplace`), not a live poke:
  the ring depth is baked at input construction and the replace path already
  handles teardown. Headless: `--framesync IDX[:FRAMES]`.
- ShowFile: `framesync` key per `inputs/` array entry (−1 off … 4), absent
  in v1 files → off.
- **Connect-backlog fill trim (found by the SRT-in bench, F3).** The §3.4
  audio model assumed ring fill ≈ prefill at steady state. Wrong for SRT: a
  connect (or reconnect) dumps the transport's buffered backlog — up to the
  SRT latency window, 100+ ms — into the ring within one mixer tick, and the
  prefill hold releases the moment fill ≥ prefill, parking a random
  [prefill, kMaxFill] of extra audio latency for the whole session. The video
  lane *drops* that same backlog (sync overflow/resync at acquire), so the
  trim demand `dVid − dAud` came out anywhere from −21 ms (negative =
  unfixable, clamps: audio cannot be advanced) to +32 ms across identical
  runs. Fix: for sync-managed lanes the mixer trims the ring to exactly
  prefill on every hold release (`InputChannel::syncManaged`); the dropped
  samples were never audible (the lane was holding). Off-mode keeps v1
  behavior (G4). Corollary: the v1 hand-set `--input-delay 0:44` for SRT
  loopback was calibrating one parking draw and is obsolete — measured
  content-level today it *adds* lateness (see `docs/bench-framesync.md`).
- **SRT-in within-session limit**: NVDEC emission burst gaps larger than the
  N-frame window step the presented video by whole frames (visible as
  ±16.7 ms av excursions); the trim intentionally chases only at 1 ms/s, so
  an excursion takes ~15 s to re-center. Deeper N absorbs more; the counters
  (`sync.starves`/`slipDrops`) make it visible. Accepted for v2 — matches
  the "not a jitter buffer" non-goal; a trim freeze during starve episodes
  is a candidate refinement.
