#!/usr/bin/env python3
"""Landmine #10 reproduction: app-switch churn -> MemoryMgr fatal alert.

Cycles the four HARDWARE app buttons (app1-app4) as fast as the screen
confirms each switch.  Within a few hundred switches the guest dies into
`blocked_on_ui` with a SysFatalAlert from MemoryMgr.c (line 4384 "Free
handle" or 4415 "Invalid handle") raised while the EMULATOR was calling
MemHandleLock — i.e. a host-initiated ROM call in the app-switch tailpatch
path (CollectCurrentAppInfo-family) used a stale handle.

Hook-independence (verified 2026-06-10): hardware-button events are not
pen/key events, so the phase2 STOP-exit EvtWakeup hook never fires during
this repro — and the same crash also reproduces on a pure master binary
driven by raw taps.  The race is pre-existing; it was unreachable before
Phase 2 only because no input ever delivered to an idle guest.

Exit status: 0 = reproduced (crash observed), 1 = no crash in N cycles.
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

PORT = 6448
BUTTONS = ["app1", "app2", "app3", "app4"]
TIMEOUT_S = 2.0
POLL_S = 0.05


def send(c, cmd):
    return (c.send_command(cmd) or "").strip()


def screen_hash(c):
    r = send(c, "screen-hash")
    parts = r.split()
    if len(parts) < 2 or parts[0] != "OK":
        return f"<{r}>"
    return parts[1]


def wait_hash_change(c, h0, timeout=TIMEOUT_S):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        h = screen_hash(c)
        if h != h0 and not h.startswith("<"):
            return h, True
        time.sleep(POLL_S)
    return h0, False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=200)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    with emulator(args.port):
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        try:
            sys.exit(run(c, args))
        finally:
            c.disconnect()


def run(c, args):
    print(f"speed -> {send(c, 'speed 100')}")
    time.sleep(2)

    h = screen_hash(c)
    switches = 0
    misses = 0
    for cycle in range(args.cycles):
        for b in BUTTONS:
            send(c, f"button {b} tap")
            h, changed = wait_hash_change(c, h)
            switches += 1
            if not changed:
                misses += 1
                st = send(c, "state")
                if "blocked" in st:
                    print(f"REPRODUCED at switch {switches} (cycle {cycle}, {b})")
                    print(f"state:  {st}")
                    print(f"dialog: {send(c, 'dialog')}")
                    return 0
        if cycle % 20 == 0:
            print(f"cycle {cycle}: {switches} switches, {misses} misses")
            sys.stdout.flush()
    print(f"NO CRASH: {switches} switches, {misses} misses")
    return 1


if __name__ == "__main__":
    main()
