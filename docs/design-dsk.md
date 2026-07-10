# Downstream keyers (feature K, v2) — design & implementation notes

Two DSK layers composited over the A/B program mix, keyed from **native
UYVA alpha inputs** (NDI + OMT). Shipped across K1 (UYVA ingest), K2
(engine/shader), K3 (GUI/persistence). Bench: `bench-dsk.md`.

## Scope decisions (locked with the operator)

- Key sources are **existing inputs carrying UYVA alpha**. No PNG stills,
  fill+key pairs, or chroma/luma self-keys in this cut (all remain natural
  extensions: the keyer shader consumes fill RGB + an alpha scalar).
- **2 keyers** (`kDskCount`), DSK2 over DSK1. Each: on/off toggle with an
  auto-fade (per-keyer duration), independent of A/B transitions.
- **FTB blacks out the keyers** (applied after keying, broadcast
  convention).
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

## Known limits / candidates

- Preview is still the raw source — there is no "preview with DSK"
  look-ahead pass (single composited program image by design; a second
  composite would be needed).
- Tie-DSK-to-transition (ATEM-style) not implemented.
- Keyer sources must be inputs; stills/media player remain v2 candidates.
