#!/usr/bin/env python3
"""Sample per-process CPU%% over a window from /proc (no perf needed).

Usage: cpu_sample.py PID [PID...] SECONDS
Prints one line per PID: "<pid> <comm> <cpu%>" (100% = one core).
"""
import os
import sys
import time


def cpu_ticks(pid):
    with open(f"/proc/{pid}/stat") as f:
        parts = f.read().rsplit(")", 1)[1].split()
    return int(parts[11]) + int(parts[12])  # utime + stime


def comm(pid):
    with open(f"/proc/{pid}/comm") as f:
        return f.read().strip()


def main():
    pids = [int(p) for p in sys.argv[1:-1]]
    dur = float(sys.argv[-1])
    clk = os.sysconf("SC_CLK_TCK")
    before = {p: cpu_ticks(p) for p in pids}
    t0 = time.time()
    time.sleep(dur)
    dt = time.time() - t0
    for p in pids:
        try:
            pct = 100.0 * (cpu_ticks(p) - before[p]) / clk / dt
            print(f"{p} {comm(p)} {pct:.1f}%")
        except FileNotFoundError:
            print(f"{p} gone")


main()
