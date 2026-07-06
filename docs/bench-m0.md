# M0 Bench Report — NDI throughput & instrumentation validation

Date: 2026-07-05 · Host: Ryzen 9 9900X (12C/24T), 62GB DDR5, RTX 5090, CachyOS
NDI SDK: 6.3.2 (2026-04-13) · Tools: moo-testgen / moo-latmeter (this repo)

## Results

| Scenario | fps | gaps | strip errors | latency avg | A/V offset | CPU (send) | CPU (recv) |
|---|---|---|---|---|---|---|---|
| 1080p59.94, 1 pair | 59.94 locked | 0 | 0 | 1.38 ms | +0.06 ms | — | — |
| 8K59.94, 1 pair | 59.94 locked | 0 | 0 | 4.5 ms | +0.06 ms | 0.72 cores | 0.31 cores |
| 8K59.94, **2 pairs simultaneously** | both 59.94 | 0 | 0 | 6.4–7.6 ms | +0.06 ms | ~0.7×2 | ~0.3×2 |

All runs: zero sender tick-skips, zero parity failures at 8K (strips survive the transport),
audio sample counts exact (800.8 samples/tick average — rational clock arithmetic verified).

## Key finding: same-host NDI bypasses SpeedHQ

The measured CPU costs are **too low to be the codec** and the 4.5 ms latency matches a
66 MB frame copy: NDI uses an **uncompressed shared-memory path for same-host connections**.
Attempts to force the codec locally all failed (numbers unchanged):

- `~/.ndi/ndi-config.v1.json` with `shared_memory.send/recv.enable=false` (key not in binary)
- `ndi.{unicast,rudp,tcp}.recv.loopback = false` (keys exist in libndi, no effect on this path)
- Spoofed `ndi.machinename` for the sender via per-process `NDI_CONFIG_DIR`
  (discovery shows the fake name, but local detection is evidently a capability handshake,
  not name matching)

`ndi.codec.shq.mode` exists but relates to SHQ0/SHQ2/SHQ7 chroma variant selection
(FourCC comparisons in the binary), not codec engagement.

## Implications

1. **Local development topology is trivially cheap.** Two 8K test sources + two receivers on
   this 12-core box cost ~2 cores total and both hold 59.94. The plan's worry about
   co-locating testgens with the switcher applies only to *codec-engaged* (remote) streams.
2. **True SpeedHQ encode/decode cost remains unmeasured** — the plan's #1 risk is still open,
   now scoped precisely: it only concerns remote NDI I/O (real cameras in, program out to
   remote receivers). Measuring it requires a genuinely remote endpoint:
   - run `moo-testgen`/`moo-latmeter` on a second box across the 10GbE link
     (link is up — carrier present on `enp104s0f1np1`), or
   - a root-created network namespace on this box (veth pair defeats the shm handshake).
3. **Latency floor** for the all-local path is ~4.5 ms at 8K (one 66 MB copy) and ~1.4 ms at
   1080p — well inside the ≤2.5-frame app budget, and this is *before* the switcher exists
   (numbers are testgen→latmeter direct).
4. Dual-pair latency rose 4.5→~7 ms: first sign of DDR5 bandwidth contention, with ample
   headroom at this load. Re-measure alongside the switcher's own ~8 GB/s in M5.

## Method

`scripts/bench_speedhq.sh [WxH] [secs] [builddir]` — starts a pair, warms up 5 s, samples
`/proc/<pid>/stat` (utime+stime, all threads) over the window. A/V offset measured from
sender-stamped timecodes: flash frame vs 1 kHz tone-burst onset (±10 ms target, reads +0.06 ms).
