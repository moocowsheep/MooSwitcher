#!/usr/bin/env python3
"""Measure A/V sync offset in a captured MooSwitcher MPEG-TS (HEVC + AAC).

Expects moo-testgen content on program: a flash region (256x16 at y=32,
white on sync frames) and a 1 kHz tone burst starting on the flash frame.
Reports onset-minus-flash offset: positive = audio late.

Capture with timestamps preserved, e.g.:
  ffmpeg -y -copyts -i "srt://HOST:9710?mode=caller&latency=120000" -c copy \
         -avoid_negative_ts disabled -muxpreload 0 -muxdelay 0 cap.ts

Timestamps are read raw via ffprobe (never through the ffmpeg filter graph,
which rebases pts and has burned us before); flashes are detected from a
cropped raw-gray decode paired with the ffprobe frame list by decode order.
The flash period is ~1.001 s, so offsets are only unambiguous within
+-500 ms -- fine once the pipeline is anywhere near calibrated.

Usage: av_offset_ts.py capture.ts
"""
import json
import struct
import subprocess
import sys


def run(args):
    return subprocess.run(args, capture_output=True, check=True)


def probe_streams(path):
    out = run(["ffprobe", "-v", "error", "-show_streams", "-of", "json", path])
    streams = json.loads(out.stdout)["streams"]
    kinds = {s["codec_type"] for s in streams}
    if kinds < {"video", "audio"}:
        print(f"FAIL: need video+audio streams, got {sorted(kinds)}")
        sys.exit(1)
    a = next(s for s in streams if s["codec_type"] == "audio")
    return float(a.get("start_time", 0.0)), int(a["sample_rate"])


def flash_times(path):
    """Raw pts of every rising edge in the flash region's brightness."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v",
               "-show_entries", "frame=best_effort_timestamp_time",
               "-of", "csv", path])
    pts = [float(line.split(",")[1]) for line in out.stdout.decode().splitlines()
           if "," in line and line.split(",")[1]]

    crop = run(["ffmpeg", "-v", "error", "-i", path, "-an",
                "-vf", "crop=256:16:0:32", "-f", "rawvideo", "-pix_fmt", "gray",
                "pipe:1"])
    block = 256 * 16
    nf = len(crop.stdout) // block

    times = []
    prev = False
    for k in range(min(nf, len(pts))):
        region = crop.stdout[k * block:(k + 1) * block]
        bright = sum(region) / block > 127
        if bright and not prev:
            times.append(pts[k])
        prev = bright
    return times


def onset_times(path, astart, rate):
    """Tone onsets, moo-latmeter's thresholds: >=50 ms quiet, 0.15 trigger,
    0.05 release, hold in between (the burst is a sine ramping from zero)."""
    p = run(["ffmpeg", "-v", "error", "-i", path, "-map", "0:a:0",
             "-ac", "1", "-f", "f32le", "pipe:1"])
    n = len(p.stdout) // 4
    samples = struct.unpack(f"<{n}f", p.stdout[: n * 4])
    onsets = []
    quiet = 1 << 30  # start armed
    for i, x in enumerate(samples):
        ax = abs(x)
        if ax > 0.15:
            if quiet >= rate // 20:
                onsets.append(astart + i / rate)
            quiet = 0
        elif ax < 0.05:
            quiet += 1
    return onsets


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    path = sys.argv[1]
    astart, rate = probe_streams(path)
    flashes = flash_times(path)
    onsets = onset_times(path, astart, rate)

    pairs = []
    for f in flashes:
        cand = min(onsets, key=lambda o: abs(o - f), default=None)
        if cand is not None and abs(cand - f) < 0.5:
            pairs.append((f, cand, (cand - f) * 1000.0))
    if not pairs:
        print(f"FAIL: no flash/onset pairs (flashes={len(flashes)} "
              f"onsets={len(onsets)})")
        sys.exit(1)

    for f, c, d in pairs:
        print(f"flash {f:9.3f}s  onset {c:9.3f}s  av {d:+7.2f} ms")
    avg = sum(d for _, _, d in pairs) / len(pairs)
    spread = max(d for _, _, d in pairs) - min(d for _, _, d in pairs)
    print(f"pairs={len(pairs)}  avg av_offset = {avg:+.2f} ms  "
          f"spread = {spread:.2f} ms  (positive = audio late)")


main()
