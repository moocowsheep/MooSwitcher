# Clean feed

Status: IMPLEMENTED (recording and NDI output, 2026-07-18).

## Operator contract

The clean feed is the switched A/B program before both downstream keyers. It
includes cuts, automatic/manual transitions, fade to black, and the same
48 kHz stereo master audio as normal program. It excludes DSK1 and DSK2 fill
and alpha, regardless of their on-air levels.

- GUI: **CLEAN REC** starts and stops an independent HEVC/AAC Matroska file.
- Headless: `--clean-record PATH.mkv`; optional
  `--clean-record-stop-after S`.
- NDI: `--clean-ndi-out NAME` enables a second sender in either executable.
- `--record-bitrate KBPS` applies to both program and clean recorders.
- Normal program recording, clean recording, normal NDI, and clean NDI can
  operate simultaneously.

Clean NDI enable/name settings persist in GUI show files. Recording paths and
active recording state remain session-only, matching normal program recording.

## GPU and output architecture

The compositor owns a second per-frame-in-flight RGBA16F target. The composite
shader calculates the A/B mix once. When a clean consumer is active, it writes
that mix with FTB to the clean target before applying DSK1/DSK2 and FTB to the
normal program target.

Each feed has independent:

- device-local UYVY pack buffers and a four-slot host readback ring for NDI;
- exportable NV12 pack buffers for NVENC recording;
- slot stamps, NDI pin tracking, recorder copied-value handshakes, and drop
  counters.

The two NDI copies share one transfer-queue submission and timeline per render
tick. Program SRT and program recording retain their existing shared NV12
buffer; clean recording uses a different NV12 buffer, so a slow clean encoder
cannot hold the program encoder buffer. All consumers remain drop-capable:
backpressure can skip an encoded-output frame but never block the render tick.

Master audio is fanned to both NDI senders and both recorder instances. DSK
sources have never been automatically mixed into program audio, so the normal
master is already the correct clean-feed audio contract. FTB dips that master
audio and both video feeds together.

## Validation

- GPU golden: an opaque green DSK over a red A/B source packed green on normal
  program and red on clean feed from the same compositor dispatch.
- Simultaneous recorder smoke: normal program and clean feed recorded together
  for 121 frames at 640×360 60000/1001. Both Matroska files finalized at
  2.052 seconds with HEVC video; normal contained the DSK and clean contained
  only the underlying source. Program rendering had zero skips.
- Clean-only NDI smoke: 122 clean frames sent for 122 render ticks with zero
  skips.
- Dual-NDI smoke: normal and clean senders each sent 92 frames for 92 render
  ticks with zero skips.
- Full automated suite: 74/74 passing.

## Known limits

- Clean feed is available as Matroska recording and NDI, not as a second SRT
  output.
- The multiview PROGRAM monitor remains normal program with DSKs; there is no
  dedicated clean monitor tile.
- Starting both recorders uses two NVENC sessions. GPU/driver session limits
  still apply.
- FTB is intentionally present on clean feed; this is a graphics-free program
  branch, not an isolated pre-master bus.
