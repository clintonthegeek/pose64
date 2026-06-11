#!/usr/bin/env python3
"""Landmine #10 regression test: app-switch churn + polling must not kill
the guest.

Cycles the four HARDWARE app buttons (app1-app4) as fast as the screen
confirms each switch.  On the broken build the guest intermittently died
into `blocked_on_ui` with a SysFatalAlert from MemoryMgr.c (line 4365
"NULL handle", 4384 "Free handle", or 4415 "Invalid handle").

ROOT CAUSE (fixed 2026-06-10): EmSession::ExecuteSubroutine aborted a
nested host-initiated ROM call when a kStopNow/kStopOnCycle suspend
(screen-hash/ui/peek/paint -> fSuspendByUIThread) arrived mid-call, so the
ROM stub's caller read garbage out of D0/A0.  App switches make ~10 host
ROM calls each (CollectCurrentAppInfo family), so churn + polling
maximized the odds.  The fix defers the suspend until the subroutine
completes (upstream POSE 3.5 semantics).

--hammer N starts N extra connections hammering screen-hash with no
delay; each is a kStopNow stop/resume cycle, which made the crash
near-reliable pre-fix (3/6 runs, switches 33-438) and is the form this
test should be run in: `--cycles 200 --hammer 3`.

Hook-independence (verified 2026-06-10): hardware-button events are not
pen/key events, so the phase2 STOP-exit EvtWakeup hook never fires during
this repro — and the same crash also reproduced on a pure master binary
driven by raw taps.

Exit status (phase-1 convention): 0 = no crash in N cycles (PASS),
1 = guest died (crash reproduced).
"""

import argparse
import os
import sys
import threading
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


def hammer_loop(port, stop_event):
    """Tight screen-hash loop on its own connection.

    Each screen-hash is an EmSessionStopper(kStopNow) stop/resume cycle on
    the worker thread.  Hammering shrinks the gap between stops so one is
    near-certain to land inside the app-switch tailpatch's nested ROM-call
    window (CollectCurrentAppInfo) — turning the intermittent crash into a
    reliable one.  (--hammer 0 = the original 20 Hz-poll behavior.)
    """
    c = ReControlClient(port=port, timeout=5)
    if not c.connect():
        return
    try:
        while not stop_event.is_set():
            c.send_command("screen-hash")
    finally:
        c.disconnect()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=200)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--hammer", type=int, default=0,
                    help="N extra connections hammering screen-hash (hot mode)")
    ap.add_argument("--log", default=None,
                    help="capture emulator stderr to this file")
    args = ap.parse_args()

    with emulator(args.port, capture_log=args.log):
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        stop_event = threading.Event()
        hammers = []
        for _ in range(args.hammer):
            t = threading.Thread(target=hammer_loop,
                                 args=(args.port, stop_event), daemon=True)
            t.start()
            hammers.append(t)
        try:
            sys.exit(run(c, args))
        finally:
            stop_event.set()
            for t in hammers:
                t.join(timeout=2)
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
                    return 1
        if cycle % 20 == 0:
            print(f"cycle {cycle}: {switches} switches, {misses} misses")
            sys.stdout.flush()
    print(f"NO CRASH: {switches} switches, {misses} misses")
    return 0


if __name__ == "__main__":
    main()
