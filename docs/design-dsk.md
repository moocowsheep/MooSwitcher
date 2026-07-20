# Downstream keyers (feature K, v2) — design & implementation notes

Two DSK layers composited over the A/B program mix, keyed from **native
alpha inputs** (NDI/OMT UYVA or local raster stills). Shipped across K1
(UYVA ingest), K2 (engine/shader), K3 (GUI/persistence), with static
PNG/WebP-style inputs added by the media layer. Bench: `bench-dsk.md`.
K4 follow-ups (2026-07-19): look-ahead preview, tie-to-transition,
audio-follow-DSK — see the sections below.

## Scope decisions (locked with the operator)

- Key sources are **existing inputs carrying alpha**. These can be live UYVA
  sources or local raster stills. No fill+key pairs or chroma/luma self-keys
  in this cut (both remain natural extensions: the keyer shader consumes fill
  RGB + an alpha scalar).
- **2 keyers** (`kDskCount`), DSK2 over DSK1. Each: on/off toggle with an
  auto-fade (per-keyer duration), independent of A/B transitions.
- **FTB blacks out the keyers** (applied after keying, broadcast
  convention).
- The clean-feed branch taps the A/B mix before both keyers, then applies FTB;
  it is documented in `design-clean-feed.md`.
- A keyed source with **no alpha renders fully opaque** — a fadeable
  fullscreen overlay. Documented behavior, not an error.

## Architecture

- **Keying is folded into `composite.comp`** (bindings 5-8: fill+aux per
  keyer), not a separate pass: at 8K a second RGBA16F read+write pass
  would add ~32 GB/s at 60 fps on this DDR5-bound box. Order in-shader:
  A/B mix → DSK1 → DSK2 → `c *= (1-ftb)`.
- **Aux binding is dual-purpose**: NV12 UV plane, UYVA alpha plane, or
  self-rebound fill — NV12 and UYVA are mutually exclusive by
  construction, signaled via flag bits in the push constants
  (bit0 NV12, bit1 alpha, bit2 premultiplied, bit3 BT.601).
- **Keying math** (level L = fade, a = key alpha, both full-range):
  straight `c = mix(c, fill, a*L)`; premultiplied
  `c = fill*L + c*(1 - a*L)`. Fill decodes through the limited-range
  `ycbcr.glsl` helpers; the alpha plane is full-range R8 (no expansion).
  Outside the keyer's aspect-fit map = transparent.
- **Premult flag is per-GpuFrame**, not per-format: OMT signals
  `OMTVideoFlags_PreMultiplied` frame-by-frame; NDI doesn't document a
  UYVA convention → defaults straight.
- `CompositePC` grew 88 → 152 bytes (over the 128 B Vulkan floor; NVIDIA
  gives 256). A runtime `maxPushConstantsSize` check aborts loudly on
  smaller devices rather than truncating.
- **SwitcherCore** owns the keyer state (`Dsk{src, level, target, dur}`),
  ramping level toward target by elapsed ticks — the FTB pattern, shared
  `lastTick_` — so skipped ticks stay rate-correct and everything is
  unit-testable.

## UYVA ingest (K1)

- `PixFmt::UYVA8_4224` = packed UYVY plane + appended full-res 8-bit
  alpha plane (`frameBytes()` = w*h*3, `alphaOffset()` = w*2*h).
- `UploadRing` slots grow an optional `R8_UNORM` alpha image + second
  copy region (the Nv12Ring two-plane pattern); `GpuFrame::viewA()`.
- NDI: `color_format_fastest` already delivers UYVA; the receiver now
  copies the appended plane (alpha row stride assumed
  `line_stride/2` — undocumented by the SDK, validated by testgen
  loopback). OMT: request `UYVYorUYVA`, accept `OMTCodec_UYVA` (libomt
  decodes at stride = width*2 with the packed alpha appended).
- **Sticky-UYVA ring**: title generators alternate UYVY/UYVA with
  content. Once a connection delivers alpha at a geometry, the UYVA ring
  is kept; plain UYVY frames memset staging alpha to 0xFF once per slot
  (per-slot opaque flag). Downgrade to a UYVY ring happens on reconnect
  or geometry change only (`alphaThisConn` per-connection eligibility) —
  bounds the extra pinned memory (8K: 99.5 MB/slot staging + alpha
  images).

## Engine policies (K2)

- **Placeholder guard**: a dead/stale key source picks the opaque-black
  placeholder — the engine forces that keyer's level to 0 in the TickJob
  copy instead of overlaying black; the overlay returns with signal. The
  keyer state machine is untouched.
- **Tally**: a keyer's source counts as program tally while engaged *or*
  still fading out (`dskOn || level > 0`). The change-detect key is a
  `uint64_t` with five 10-bit fields (pgmA, pgmB, pvw, dsk1, dsk2).
  Multiview shows the red border on DSK sources.
- Upload waits and `retention_` already covered all inputs via the
  multiview row; DSK frames add explicit (dedup'd) waits.
- **No audio-follow**: a DSK source's audio is whatever its mixer channel
  does — fade the fader by hand if the graphic has sound. (Candidate
  refinement: audio-follow-DSK.)

## Operator surface (K3)

- GUI row under the transition row: per keyer an on-air toggle button
  (FTB-red when engaged), key-source combo, fade-duration spinbox.
  Shortcuts: `D` = DSK1, `Shift+D` = DSK2.
- Headless: `--dsk K:SRC`, `--dsk-fade K:TICKS`, `--dsk-toggle-after S:K`.
- ShowFile: `[dsk]` array (`source`, `fadeDurTicks`, `on`), absent in
  pre-DSK files → off. A restored `on=true` keyer **fades in on
  startup**.
- testgen: `--uyva` bakes an opaque strip band (counters stay
  machine-readable through the keyer), a solid-blue lower-third band
  with soft alpha edge ramps, transparent elsewhere; `--premult`
  premultiplies the fill (and sets the OMT flag — NDI has no flag, so a
  premult NDI feed keys with straight math by design; it's a test tool).

## Validation

- `switcher_core_test`: ramp/clamp, mid-fade reversal, rate-correctness
  across skipped ticks, independence from cut/auto/FTB, per-keyer
  durations, index bounds.
- `gpu_composite_test` "DSK keying": straight/premult blend values,
  no-alpha opaque fallback, half-level, FTB-over-key = black — through
  the real composite→proxy→tile path.
- Headless e2e PPM sequences + offscreen GUI screenshot with a show-file
  restored keyer (band on program, red button, red borders).
- 8K gate in `bench-dsk.md`: keyers-off delta ≈ 0; an 8K UYVA key over an
  8K program holds 59.94 (dominant cost = the key input's +50% upload
  bandwidth).

## Look-ahead preview (K4)

- The multiview PREVIEW monitor is now a real composite: preview bus +
  both keyers at their **post-next-transition** state (a tied keyer shows
  toggled, an untied one shows its current engaged state; binary levels —
  no animation). FTB never darkens it.
- Rendered by a **second `composite.comp` dispatch at proxy resolution**
  (the `proxyUsed` extent inside a 960×544 RGBA16F target, ~1/144 of the
  8K pixel count) — the full-res second pass that was ruled out
  (~32 GB/s at 8K) stays ruled out; the operator approved the lower-res
  preview. Same pipeline/descriptors with the b-bus source bound as A,
  alpha 0, `cleanEnabled` 0; the multiview tile samples it directly
  (mode 1), replacing the old raw input-proxy preview tile.
- Keyer fills are sampled at full res by the proxy-size dispatch
  (bilinear undersampling aliasing accepted for a monitor).
- The placeholder guard applies: a dead key source previews dark, not
  opaque black. `TickJob::pvwDskLevel[]` carries the guarded levels.

## Tie-to-transition (K4)

- Per-keyer `tie` flag (ATEM "next transition" convention), state in
  `SwitcherCore::Dsk{tie, tieRun, tieFrom}`:
  - Arming an AUTO or grabbing the T-bar snapshots `tieFrom = level` and
    commits `target` to the toggled state; while the transition runs the
    keyer's level = `mix(tieFrom, target, transition progress)` — it
    rides the transition's clock, not its own fade duration.
  - CUT with no transition running executes the toggle instantly (snap
    with the bus swap); CUT mid-transition lands keyers with the buses.
  - A canceled transition (bus assign) reverts `target` to the
    pre-transition state; the keyer ramps back at its **own** fade rate.
  - Re-grabbing a held T-bar does NOT re-arm (guard in `tbarBegin`);
    `dskToggle` during a tied run redirects the destination mid-flight.
- Untied keyers are unchanged: independent fades, own duration.

## Audio-follow-DSK (K4)

- Per-keyer `audioFollow` flag. The render thread publishes each
  followed keyer's **guarded on-screen level** + source to the audio
  engine (`publishDsk`, a second packed atomic beside the bus snapshot);
  `MixerCore` gives that input at least `level` of bus gain,
  **max-combined** with the A/B crossfade so a source that is both
  program and key never doubles. Solo still bypasses everything.
- Follow rides the same level the viewer sees: linear (not equal-power)
  and forced silent by the placeholder guard when the key source dies.

## Operator surface additions (K4)

- GUI keyer cards: TIE and AUD FOLLOW toggle buttons (amber when on).
- Headless: `--dsk-tie K`, `--dsk-afv K`.
- ShowFile `[dsk]`: `tie`, `audioFollow` keys (absent = off).
- Remote control: `DSK k TIE|AFV [ON|OFF|TOGGLE]` (bare form toggles;
  `FOLLOW` = alias for AFV); state JSON keyers gained `tie`/`afv`.
  Companion module: `dsk_tie`/`dsk_afv` actions + feedbacks + presets.

## Known limits / candidates

- Keyer sources must be inputs; local alpha stills can occupy those inputs.
- Look-ahead preview is monitor-resolution only (by design; see above) and
  shows tied keyers at full level, not a transition rehearsal.
