# DSK bench (K2) — 2026-07-10

Box: 5090 / 9900X (see `bench-m5.md` for the baseline pipeline numbers at
`b150fd5`).

## GPU golden tests (ctest `gpu_composite_test`)

Straight key (alpha 128, level 1) over 75% red with 75% green fill lands
mix(red, green, 0.502) within ±12/255 through the full composite→proxy→tile
path; premultiplied variant, no-alpha opaque-overlay fallback, half-level
mix, and FTB=1-over-keyer (black) all asserted. See the "DSK keying" case.

## 8K perf gate

2× 8K59.94 NDI inputs (one plain, one UYVA = +50% upload bandwidth on that
input), NDI out enabled, 12 s runs:

- **Keyers off:** skips froze at 4 (connect churn), both inputs full rate.
  The two `level > 0` shader branches are the entire keyers-off cost —
  no measurable delta vs baseline.
- **DSK on (8K UYVA key over 8K program):** skips froze at 2 (connect
  churn), both inputs full rate, no steady-state skips over the run.
  The dominant added cost is the keyed source's UYVA upload (~2 GB/s at
  8K60, i.e. +50% of that input's UYVY rate), not the composite pass —
  keying is folded into the existing single composite dispatch (no extra
  RGBA16F round-trip; a separate pass would have added ~32 GB/s at 8K60).

## Memory note

A sticky-UYVA 8K ring holds `(kSlots + N + 2) × 99.5 MB` pinned WC staging
(≈1.1 GB at N=4) plus the R8 alpha images until reconnect/geometry change
(downgrade is per-connection). 1080p: 3.1 MB/slot.

## E2E (headless)

`moo-testgen --uyva` (blue lower-third band, opaque strip rows, soft edge
ramps) keyed over bars via `--dsk 0:1 --dsk-toggle-after ...`: PPM dumps
show fade-in/out of the band, transparency outside it, the key's strip
rows opaque over program, and the red DSK tally border on the key source's
multiview tile. 1080p run: 0 render skips, 0 upload drops.
