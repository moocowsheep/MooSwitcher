# MooSwitcher

Controls a [MooSwitcher](https://github.com/moocowsheep/MooSwitcher) live
video switcher over its TCP remote-control port.

## Setup

1. Start the MooSwitcher GUI (remote control listens on port **9923** by
   default; change with `--control-port N`, disable with `--control-port 0`).
   For headless shows, start `moo-headless` with `--control-port 9923`.
2. Add this connection in Companion with the switcher's IP address and port.

## Available controls

- **Buses**: program/preview select (1-based, matching the GUI), cut, auto,
  fade to black, transition type/duration.
- **DSKs**: on/off/toggle, fill source, fade duration.
- **Media inputs**: play/pause/restart, playlist next/previous, loop.
- **Recording**: program and clean-feed record start/stop/toggle with
  optional path (default: timestamped file in `~/Videos`).
- **Audio**: per-input mute/solo/fader.

Feedbacks provide program (red) and preview (green) tally per input, DSK
on-air, FTB, recording, SRT-connected, input-signal-lost, and mute states.
Variables expose program/preview names, record timecodes, and every input
label. Ready-made buttons are in the presets library under Program,
Preview, and Transport.
