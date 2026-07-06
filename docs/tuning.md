# Tuning for 8K / remote sources

The switcher itself needs no tuning for same-host validation (see
`docs/bench-m5.md`: full 8K pipeline < 2 cores, zero overruns untuned).
This page is for **remote** NDI/SRT at high bitrates and for hosts with
less headroom.

## Network buffers (needs sudo)

Remote 8K NDI is 1–2.5 Gbps; SRT at 120 ms latency wants deep buffers.
Defaults on Arch (4 MB rmem_max) drop bursts long before the wire is full.

```sh
sudo cp scripts/tuning-sysctl.conf /etc/sysctl.d/90-mooswitcher.conf
sudo sysctl --system
```

SRT URLs: `latency` is in **microseconds** in FFmpeg URLs (the GUI shows ms).
Leave `maxbw` unset (libsrt's live-mode default ~1 Gbps cap is fine for
80–200 Mbps); never set `maxbw=0` without an explicit `inputbw`.

## Same-host topology limits (measured, 9900X + RTX 5090)

Same-host NDI is uncompressed shared memory: every 8K60 hop moves ~4 GB/s
of pixels through system RAM. This box sustains **one full 8K pipeline**
(testgen → switcher → NDI out + SRT out → latmeter ≈ 5 streams ≈ 20 GB/s)
cleanly. Two 8K inputs *plus* an 8K NDI out *plus* a local 8K receiver
saturates memory bandwidth (stale frames, gaps). For 2×8K production use,
source cameras from the network, not from local senders — the plan's
two-box topology.

## CPU affinity

Not needed at current load (render+mixer+capture < 2 cores of 24, zero
overruns in the 30-min soak). If a smaller host shows `render.skips` or
`audio.skips` in the counters, pin the noisy neighbors (browsers,
compilers) away from the app rather than pinning the app: the NDI SDK
manages its own thread pool and reacts badly to a shrunken cpuset.

## The SpeedHQ unknown (remote NDI codec cost)

Same-host runs never touch the SpeedHQ codec (shm carries uncompressed
frames — verified: forcing TCP-only in ndi-config does not disengage the
shm path). Codec cost therefore only appears with a remote peer:

- Real bench: second machine on the 10 GbE link running `moo-testgen`.
- One-machine bench: `sudo scripts/ndi-netns-bench.sh up` puts a testgen
  in a network namespace so NDI treats it as remote (veth carries
  >10 Gbps; the codec, not the wire, is the bottleneck). Measure with
  `scripts/cpu_sample.py`.

NDI's own guidance: memory bandwidth binds before the codec on modern
CPUs. Until measured, budget 2–4 cores for an 8K SpeedHQ decode.

## Clock discipline

NDI timecodes ride the realtime clock; NTP slew (systemd-timesyncd,
chrony) shows up as a few ppm of apparent A/V drift in
timecode-comparing measurements and in timestamp-driven playout over
hours. The pipeline itself is locked to CLOCK_MONOTONIC and does not
drift. If downstream playout syncs by NDI timestamps across hours, use
PTP instead of NTP.
