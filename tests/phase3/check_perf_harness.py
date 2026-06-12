#!/usr/bin/env python3
"""Landmine #7 measurement + acceptance harness.

Phases:
  baseline   3 min, all check flags off, gremlin running
  flagged    N min (default 10), DRAM check flag(s) on, same gremlin load

Samples every 10s: emulator CPU% (from /proc/<pid>/stat deltas), RSS,
and ReControl `state` round-trip latency.  Writes CSV to --csv (default
/tmp/check_perf.csv) and prints a summary.

Modes:
  (default)      measurement only — prints numbers, exits 0
  --acceptance   asserts the spec bars and exits nonzero on failure:
                   * CPU%: median(flagged) <= median(baseline) + CPU_MARGIN
                     (the load-bearing check — the freeze is INSTANT, so a
                      within-flagged-phase drift check false-passes a frozen
                      CPU; this compares flagged against the un-flagged
                      baseline instead — Phase 3c spec-review strengthening)
                   * CPU%: median of last 2 min <= median of first 2 min + 10
                     (kept as a secondary drift guard)
                   * every latency sample < 2.0 s
                   * RSS growth across flagged phase < 10 MB
  --flags X,Y    which flags to enable (default ScreenAccess; acceptance runs
                 use all six DRAM flags)
  --minutes N    flagged-phase length (default 10)
"""

import argparse
import csv
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6427
DRAM_FLAGS = ["LowMemoryAccess", "SystemGlobalAccess", "ScreenAccess",
              "MemMgrDataAccess", "FreeChunkAccess", "UnlockedChunkAccess"]
CLK_TCK = os.sysconf("SC_CLK_TCK")


def cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        parts = f.read().split()
    return (int(parts[13]) + int(parts[14])) / CLK_TCK


def rss_mb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024.0
    return 0.0


def sample_phase(proc, c, label, minutes, rows):
    t_end = time.time() + minutes * 60
    last_cpu, last_t = cpu_seconds(proc.pid), time.time()
    while time.time() < t_end:
        time.sleep(10)
        now = time.time()
        cpu = cpu_seconds(proc.pid)
        pct = 100.0 * (cpu - last_cpu) / (now - last_t)
        last_cpu, last_t = cpu, now
        t0 = time.time()
        r = c.send_command("state") or "TIMEOUT"
        lat = time.time() - t0
        rows.append({"phase": label, "t": round(now, 1), "cpu_pct": round(pct, 2),
                     "rss_mb": round(rss_mb(proc.pid), 1), "lat_s": round(lat, 3),
                     "state": r.strip()[:40]})
        print(rows[-1])


def median(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--acceptance", action="store_true")
    ap.add_argument("--flags", default="ScreenAccess")
    ap.add_argument("--minutes", type=int, default=10)
    ap.add_argument("--csv", default="/tmp/check_perf.csv")
    ap.add_argument("--emulog", default=None,
                    help="capture emulator stdout/stderr to this file")
    args = ap.parse_args()
    flags = DRAM_FLAGS if args.flags == "all" else args.flags.split(",")

    rows = []
    with emulator(PORT, capture_log=args.emulog) as proc:
        c = connect(PORT, timeout=10)
        try:
            r = c.send_command("gremlin new 42 2000000")
            assert r.startswith("OK"), f"gremlin failed: {r}"
            sample_phase(proc, c, "baseline", 3, rows)
            for fl in flags:
                r = c.send_command(f"check set {fl} on")
                assert r.startswith("OK"), f"check set {fl} failed: {r}"
            sample_phase(proc, c, "flagged", args.minutes, rows)
            c.send_command("check clearall")
            c.send_command("gremlin stop")
        finally:
            c.disconnect()

    with open(args.csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)

    # CPU_MARGIN: the flagged-vs-baseline allowance.  Pre-fix the delta is
    # ~60pp (baseline ~40%, flagged pinned ~100%); a correct fix brings flagged
    # CPU to within sampling noise of baseline.  15pp sits well above that noise
    # band yet ~45pp below the pre-fix delta, so it cleanly separates "fixed"
    # from "frozen" — a CPU still pinned by the freeze fails by a wide margin.
    CPU_MARGIN = 15

    baseline = [r for r in rows if r["phase"] == "baseline"]
    flagged = [r for r in rows if r["phase"] == "flagged"]
    baseline_cpu = [r["cpu_pct"] for r in baseline]
    flagged_cpu = [r["cpu_pct"] for r in flagged]
    first2 = [r["cpu_pct"] for r in flagged if r["t"] <= flagged[0]["t"] + 120]
    last2 = [r["cpu_pct"] for r in flagged if r["t"] >= flagged[-1]["t"] - 120]
    lat_max = max(r["lat_s"] for r in flagged)
    rss_growth = flagged[-1]["rss_mb"] - flagged[0]["rss_mb"]
    print(f"\nflags={flags}")
    print(f"cpu% baseline median={median(baseline_cpu):.1f} "
          f"flagged median={median(flagged_cpu):.1f} "
          f"(margin {CPU_MARGIN}pp)")
    print(f"cpu% first2min median={median(first2):.1f} last2min median={median(last2):.1f}")
    print(f"latency max={lat_max:.3f}s  rss growth={rss_growth:.1f}MB  csv={args.csv}")

    if args.acceptance:
        cpu_vs_baseline = median(flagged_cpu) <= median(baseline_cpu) + CPU_MARGIN
        cpu_drift = median(last2) <= median(first2) + 10
        lat_ok = lat_max < 2.0
        rss_ok = rss_growth < 10.0
        ok = cpu_vs_baseline and cpu_drift and lat_ok and rss_ok
        print(f"  cpu_vs_baseline={cpu_vs_baseline} cpu_drift={cpu_drift} "
              f"lat_ok={lat_ok} rss_ok={rss_ok}")
        print("ACCEPTANCE", "PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
