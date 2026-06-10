#!/usr/bin/env python3
"""Phase 2 idle-CPU harness (Q-IDLE).

Boots m515.psf to wherever the session left off (launcher), applies a speed,
waits SETTLE_S with zero input, then samples the pose64 process's CPU% over
DURATION_S via /proc/<pid>/stat (utime+stime deltas).  Prints the mean.

Caveats recorded with the numbers (handoff §10 Q-IDLE): offscreen platform
(no real paint cost); Max idle pegs a core at baseline by design
(EmCPU68K.cpp STOP-loop sleep is speed>0-gated) — A's cost is only
meaningful at 1x.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "tests"))
from phase1._harness import emulator  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from test_recontrol import ReControlClient  # noqa: E402

PORT = 6442
CLK_TCK = os.sysconf("SC_CLK_TCK")


def proc_cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(") ", 1)[1].split()
    utime, stime = int(fields[11]), int(fields[12])  # fields 14,15 of stat
    return (utime + stime) / CLK_TCK


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", default="100")
    ap.add_argument("--settle", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    with emulator(args.port) as proc:
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        print(c.send_command(f"speed {args.speed}"))
        c.disconnect()  # no open client during measurement — true idle

        time.sleep(args.settle)
        cpu0, t0 = proc_cpu_seconds(proc.pid), time.time()
        time.sleep(args.duration)
        cpu1, t1 = proc_cpu_seconds(proc.pid), time.time()

        pct = 100.0 * (cpu1 - cpu0) / (t1 - t0)
        print(f"idle CPU: {pct:.2f}%  (speed={args.speed}, "
              f"settle={args.settle}s, window={t1 - t0:.0f}s, offscreen)")


if __name__ == "__main__":
    main()
