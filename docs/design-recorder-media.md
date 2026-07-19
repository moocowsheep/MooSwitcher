# Program recorder and media player

Status: IMPLEMENTED (first production slice, 2026-07-18).

## Scope

- Program recording to Matroska: HEVC/NVENC video and optional 48 kHz stereo
  AAC master audio.
- A local H.264/HEVC clip can occupy any normal input slot. It participates in
  program/preview, transitions, DSK selection, tally, the audio mixer, and show
  persistence like a network source.
- Per-media-input play/pause, restart, loop, position, and duration controls.
- Program recording and media input are available in both GUI and headless
  operation.

Playlists, trim points, variable speed, still images, ISO recording, replay,
and clean-feed recording are intentionally later layers.

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
- decoded NV12 frames and resampled stereo audio enter the existing GPU
  mailbox and mixer lane.

The show file stores the media path and loop choice. Pause is deliberately
ephemeral: restoring a show cues the clip at its beginning and starts playback.

## Validation

- Unit: show-file round trip preserves `type=media`, path, and loop while
  resetting ephemeral pause state.
- Recorder smoke: two-second 1080p59.94 recording, zero render skips.
  `ffprobe`: HEVC 1920x1080 at 60000/1001 plus AAC stereo 48 kHz; finalized
  duration approximately two seconds.
- Playback smoke: that recording re-entered through `--media-input`, decoded
  at 60000/1001 with zero render skips, and delivered 60 frames in its first
  second.
- Full automated suite: 66/66 passing after integration.

## Known limits

- Local video decode currently requires an FFmpeg decoder that exposes CUDA
  frames; H.264 and HEVC are the supported production formats.
- One media player consumes one normal input slot.
- Loop boundaries can re-arm the audio prefill and are not promised gapless.
- SRT and recording share the compositor's single NV12 pack buffer. Either
  encoder falling behind can make both encoded outputs skip that frame, but
  program rendering remains unaffected.
- Matroska is the only recording container in this cut; HEVC bitrate is auto
  selected unless `--record-bitrate` is supplied.
