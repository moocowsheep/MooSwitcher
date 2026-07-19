# Program recorder and media player

Status: IMPLEMENTED (recorder, player, playlist, trim, and speed layers,
2026-07-18).

## Scope

- Program recording to Matroska: HEVC/NVENC video and optional 48 kHz stereo
  AAC master audio.
- An ordered playlist of local H.264/HEVC clips can occupy any normal input
  slot. It participates in
  program/preview, transitions, DSK selection, tally, the audio mixer, and show
  persistence like a network source.
- Per-media-input previous/next, play/pause, restart, whole-list loop, position,
  duration, and current-item controls.
- Per-item inclusive in and exclusive out points, in milliseconds relative to
  the source timeline; an unset out point plays through EOF.
- Per-item 0.25×–4.00× playback with pitch-preserving audio tempo.
- Program recording and media input are available in both GUI and headless
  operation.

Still images, ISO recording, replay, and clean-feed recording are intentionally
later layers.

## Recorder architecture

The compositor already creates one exportable NV12 buffer per frame-in-flight
for SRT. `FileRecorder` imports those same buffers into a separate NVENC
session. The render thread packs NV12 once and posts the completed timeline
value to every ready consumer; a buffer is not reused until SRT and the
recorder have each copied their prior event.

Encoding and Matroska I/O have bounded SPSC queues and run on dedicated
threads. A full queue drops recorder work and increments counters; it never
blocks the render or audio clocks. The audio mixer takes atomic shared
ownership of the active recorder before pushing a master chunk, so live
start/stop cannot leave a dangling sink.

CUDA is warmed before the live media clock starts. Recorder construction runs
on the requesting control thread, keeping NVENC session startup off the render
thread. Stopping drains accepted GPU events and encoded packets, writes the
Matroska trailer, then releases the session.

## Media-input architecture

`MediaInput` specializes the existing FFmpeg/NVDEC input:

- demux and video/audio decode remain off the render thread;
- source timestamps pace local-file reads against `CLOCK_MONOTONIC`;
- pause shifts that pacing anchor, so resume never races to catch up;
- restart/loop reopen the file and reset decoder/pacing state;
- end-of-file advances to the next item; final end-of-file either stops or
  wraps to item one according to whole-list loop mode;
- a trimmed item seeks backward to the nearest demuxer keyframe, decodes and
  discards pre-roll, presents its first frame at or after the inclusive in
  point, and advances before presenting the exclusive out frame;
- source timestamps are mapped to wall time by the item's playback rate;
  FFmpeg `atempo` filters are chained as needed to cover the full 0.25×–4.00×
  range, then `aformat` restores the mixer contract (48 kHz stereo float);
- operator previous/next wraps independently of automatic loop mode, so the
  list remains navigable after it stops;
- decoded NV12 frames and resampled stereo audio enter the existing GPU
  mailbox and mixer lane.

Changing items reuses the same decode thread and closes/reopens FFmpeg state
off the render thread. A format change rebuilds the existing NV12 upload ring,
and frame sync sees the timestamp discontinuity and re-locks normally. An
unreadable item advances according to the same stop/wrap policy after the
bounded open retry.

The show file stores playlist order, per-item trim points and speed, and the
whole-list loop choice. Pause and current item are deliberately ephemeral:
restoring a show cues item one at its in point and starts playback. Older
single-path and pre-speed playlist show files load with `IN=0`, `OUT=END`,
and `1.00×`.

## Validation

- Unit: playlist policy covers middle-item advance, final-item stop/wrap, and
  manual previous/next wrap, plus inclusive-in/exclusive-out edge policy and
  invalid-range normalization. Rate mapping covers 0.25×, 0.5×, 1×, 2×, and
  4×.
- Unit: show-file round trip preserves ordered media paths, trim points, speed,
  and loop mode while resetting ephemeral pause/current-item state.
- Recorder smoke: two-second 1080p59.94 recording, zero render skips.
  `ffprobe`: HEVC 1920x1080 at 60000/1001 plus AAC stereo 48 kHz; finalized
  duration approximately two seconds.
- Playback smoke: that recording re-entered through `--media-input`, decoded
  at 60000/1001 with zero render skips, and delivered 60 frames in its first
  second.
- Playlist smoke: two finalized HEVC/AAC recordings played in order through
  one input with loop off; both opened, all 163 video frames were delivered,
  playback stopped after item two, and program rendering had zero skips.
- Trim smoke: the same clips used ranges `300–800 ms` and `200–900 ms`;
  playback opened both, delivered the expected 72 video frames, stopped after
  item two, and program rendering had zero skips.
- Speed smoke: those ranges played at 2.00× then 0.50× with pitch-preserving
  audio enabled; all 72 source frames were processed, the expected accelerated
  mailbox skips/slow-motion repeats stayed confined to the input, and program
  rendering had zero skips.
- Boundary smoke: chained tempo filters negotiated and played correctly at
  both 4.00× and 0.25×; the 4× source naturally outpaced the 60p upload/render
  sampling (counted, non-blocking input drops), while program rendering again
  had zero skips.
- Full automated suite: 71/71 passing after speed integration.

## Known limits

- Local video decode currently requires an FFmpeg decoder that exposes CUDA
  frames; H.264 and HEVC are the supported production formats.
- One media player consumes one normal input slot.
- Playlist and loop boundaries can re-arm the audio prefill and are not
  promised gapless.
- Video trim edges are frame-accurate. Audio trim edges follow decoded audio
  frame boundaries (typically within one AAC block).
- Speed changes are item-boundary controls; there is no live rate ramp within
  a playing item.
- SRT and recording share the compositor's single NV12 pack buffer. Either
  encoder falling behind can make both encoded outputs skip that frame, but
  program rendering remains unaffected.
- Matroska is the only recording container in this cut; HEVC bitrate is auto
  selected unless `--record-bitrate` is supplied.
