#!/usr/bin/env python3
"""Phase 2 delivery test (recovery-plan 2.1, REVISED) — THE referee.

Targets the idle->deliver effect head-on (handoff §11.4): from the launcher
(guest idle in evtWaitForever/STOP), `tap` the Date Book icon and require the
Datebook day view to actually appear; restore Home with `key 264`.  Both legs
exercise the wake mechanism (pen and key paths).

Effect-based (R3): a tap counts as delivered only if the Datebook form is
CONFIRMED on screen (via `ui`) within TIMEOUT_S.  Screen-hash alone is not
trusted for the success edge — the launcher clock title repaints on minute
boundaries and would inflate the delivery count.  Asserts nothing about
response strings (they change in Phase 2); logs them for diagnostics.

Latency caveat: "latency" here is end-to-end (tap sent -> Datebook form
confirmed), which includes Datebook's own launch time and the poll
granularity — it is an upper bound on delivery latency.

Baseline note (handoff §11 + 2026-06-10 wedge discovery): the original
m515.psf was saved in a wedged state (guest spinning in a supervisor ROM
delay loop, SR intmask=6, timer interrupt masked — ExecuteStoppedLoop never
entered) where NO input could ever deliver: baseline = 0%.  This test runs
against the re-saved healthy session, which idles in STOP as designed.

Modes:
  rapid : taps back-to-back (awake/just-active guest path)
  idle  : sleep IDLE_S before each tap so the guest is asleep in STOP
          (exercises failure mode #1 — the cold-asleep first-contact case)
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

PORT = 6441

# Date Book icon center in the saved launcher state (verified live 2026-06-10;
# top of the "All" category page).  The psf pins the launcher scroll position,
# so this is stable across runs.
ICON_X, ICON_Y = 74, 67

# Form signatures ('ui' substrings).  Launcher and Datebook are BOTH form
# id=1000, so identify by distinctive buttons.  The launcher signature pins
# the "All" category: button 1004 shows the CURRENT category name, and the
# icon grid layout (hence ICON_X/Y) is only known for "All".
DATEBOOK_SIG = '"Go To"'
LAUNCHER_SIG = 'BUTTON id=1004 "All"'

TIMEOUT_S = 2.0     # delivery deadline per tap
RESTORE_S = 5.0     # restore deadline per key 264
POLL_S = 0.05
IDLE_S = 1.0


def send(c, cmd):
    return (c.send_command(cmd) or "").strip()


def screen_hash(c):
    r = send(c, "screen-hash")          # "OK <crc32> <w> <h>"
    parts = r.split()
    assert len(parts) >= 2 and parts[0] == "OK", f"screen-hash failed: {r!r}"
    return parts[1]


def read_ui(c):
    """`ui` is multi-line, '.'-terminated; errors are a single ERR line."""
    c.socket.sendall(b"ui\n")
    buf = b""
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        buf += c.socket.recv(65536)
        if buf.startswith(b"ERR") and b"\n" in buf:
            return buf.decode("utf-8", "replace")
        if b"\n.\n" in buf or buf.endswith(b".\n"):
            return buf.decode("utf-8", "replace")
    raise AssertionError("ui response did not terminate")


def wait_delivered(c, h0, timeout=TIMEOUT_S):
    """True once the Datebook form is confirmed on screen.

    Cheap hash polling for the trigger; `ui` only to confirm, so a launcher
    clock repaint cannot be miscounted as delivery.
    """
    deadline = time.monotonic() + timeout
    h = h0
    while time.monotonic() < deadline:
        nh = screen_hash(c)
        if nh != h:
            if DATEBOOK_SIG in read_ui(c):
                return True
            h = nh                      # unrelated repaint; keep watching
        else:
            time.sleep(POLL_S)
    return False


def restore_launcher(c):
    """Drive back to the launcher, "All" category, icons in known layout.

    key 264 (vchrLaunch) goes Home from inside an app — but pressed while
    ALREADY in the launcher it cycles to the next category.  So: re-check
    state before every press and only succeed on launcher+All.  A wrong
    category self-heals (264 cycles back around to All).
    """
    deadline = time.monotonic() + 15.0
    while time.monotonic() < deadline:
        if LAUNCHER_SIG in read_ui(c):
            return True
        send(c, "key 264")
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["rapid", "idle"], default="rapid")
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--speed", default="100", help="percent or 'max'")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    with emulator(args.port):
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        try:
            run(c, args)
        finally:
            c.disconnect()


def run(c, args):
    print(f"speed -> {send(c, f'speed {args.speed}')}")

    # Setup: the psf boots to the launcher; require it before measuring.
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if LAUNCHER_SIG in read_ui(c):
            break
        time.sleep(0.2)
    else:
        raise AssertionError("setup: launcher never appeared after boot")

    delivered = 0
    failures = []
    latencies = []
    responses = {}
    for i in range(args.count):
        if args.mode == "idle":
            time.sleep(IDLE_S)
        h0 = screen_hash(c)
        t0 = time.monotonic()
        resp = send(c, f"tap {ICON_X} {ICON_Y}")
        responses[resp] = responses.get(resp, 0) + 1
        if wait_delivered(c, h0):
            delivered += 1
            latencies.append(time.monotonic() - t0)
        else:
            failures.append(i)
        if not restore_launcher(c):
            print(f"ABORT: unrecoverable UI state at iteration {i}")
            print(f"  state:  {send(c, 'state')}")
            print(f"  dialog: {send(c, 'dialog')}")
            break

    n = len(latencies)
    lat = sorted(latencies)
    print(f"mode={args.mode} speed={args.speed} count={args.count}")
    print(f"delivered={delivered}/{args.count} "
          f"({100.0 * delivered / max(1, args.count):.1f}%)")
    if n:
        print(f"latency p50={lat[n // 2] * 1000:.0f}ms "
              f"p95={lat[min(n - 1, int(n * 0.95))] * 1000:.0f}ms "
              f"max={lat[-1] * 1000:.0f}ms")
    print(f"failures at: {failures[:20]}")
    print(f"responses: {responses}")


if __name__ == "__main__":
    main()
