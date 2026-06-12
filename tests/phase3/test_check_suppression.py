#!/usr/bin/env python3
"""Landmine #7: arming a DRAM check flag must not pin the CPU.

Effect test (pre-arm vs post-arm CPU), closing the acceptance-harness hole
where an INSTANT pin passes its within-flagged-phase drift bar.

Pre-fix: arming SystemGlobalAccess under gremlin load pins the CPU worker
to ~100% instantly (one hot PC re-analyzed per access).  Post-fix: the
site is analyzed once, repeats are suppressed, CPU stays near baseline.

PASS bars:
  * post-arm CPU median <= pre-arm median + 15pp  (two 30s windows)
  * every `state` round-trip < 2s throughout
  * re-arm (clearall + set) does not pin either (invalidation works,
    re-analysis is once-per-site)
Exit 0 = PASS, 1 = FAIL (phase-1 repro convention).
"""

import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402
from tests.phase3.check_perf_harness import cpu_seconds, median  # noqa: E402

PORT = 6428


def cpu_window(proc, c, seconds, label):
    """Sample CPU% every 5s for `seconds`; assert state stays responsive.

    A blocked_on_ui state is the once-per-arming violation dialog: respond
    `continue` and count.  Pre-fix, every continue re-blocks within ms (the
    next access re-reports), so responses ~= samples; post-fix a handful of
    distinct sites report once each.  Returns (cpu_median, responses).
    """
    samples = []
    states = set()
    responses = 0
    last_cpu, last_t = cpu_seconds(proc.pid), time.time()
    t_end = time.time() + seconds
    while time.time() < t_end:
        time.sleep(5)
        now = time.time()
        cpu = cpu_seconds(proc.pid)
        samples.append(100.0 * (cpu - last_cpu) / (now - last_t))
        last_cpu, last_t = cpu, now
        t0 = time.time()
        r = c.send_command("state")
        lat = time.time() - t0
        assert r is not None and lat < 2.0, \
            f"{label}: state round-trip {lat:.2f}s (r={r!r})"
        r = (r or "").strip()
        states.add(r)
        if "blocked_on_ui" in r:
            resp = c.send_command("dialog respond continue")
            if resp and resp.startswith("OK"):
                responses += 1
    m = median(samples)
    print(f"{label}: cpu median {m:.1f}% samples={[round(s, 1) for s in samples]}"
          f" states={sorted(states)} dialog_continues={responses}")
    return m, responses


def main():
    failures = []
    with emulator(PORT) as proc:
        c = connect(PORT, timeout=10)
        try:
            r = c.send_command("gremlin new 42 2000000")
            assert r.startswith("OK"), f"gremlin failed: {r}"
            time.sleep(5)

            pre, _ = cpu_window(proc, c, 30, "pre-arm")

            r = c.send_command("check set SystemGlobalAccess on")
            assert r.startswith("OK"), f"check set failed: {r}"
            post, post_resp = cpu_window(proc, c, 30, "post-arm")
            if post > pre + 15.0:
                failures.append(f"armed: cpu {pre:.1f}% -> {post:.1f}% (pin)")
            if post_resp >= 5:
                failures.append(
                    f"armed: {post_resp} dialog continues in 30s (re-reporting)")

            r = c.send_command("check clearall")
            assert r.startswith("OK"), f"clearall failed: {r}"
            r = c.send_command("check set SystemGlobalAccess on")
            assert r.startswith("OK"), f"re-arm failed: {r}"
            rearm, rearm_resp = cpu_window(proc, c, 30, "re-arm")
            if rearm > pre + 15.0:
                failures.append(f"re-armed: cpu {pre:.1f}% -> {rearm:.1f}% (pin)")
            if rearm_resp >= 5:
                failures.append(
                    f"re-armed: {rearm_resp} dialog continues in 30s (re-reporting)")

            c.send_command("check clearall")
            c.send_command("gremlin stop")
        finally:
            c.disconnect()

    if failures:
        print("FAIL:", "; ".join(failures))
        sys.exit(1)
    print("PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
