#!/usr/bin/env python3
"""Repro 1.8 — WorkerDirect argument errors are swallowed.

Mechanism: kCmdWorkerDirect commands (tap/pen/key/type/button) are dispatched
through QueueWork, which hardcodes the "OK\\n" response (ReControl.cpp).  The
handler's own "ERR usage" return value is discarded, and QString::toInt()
silently yields 0 on non-numeric input.  So malformed input returns OK and is
fire-and-forgotten.

Fix: validate arguments on the MAIN thread before queueing so malformed input
returns "ERR usage" immediately; well-formed input still returns OK and lands
(effect: a real tap changes the screen).

PRE-FIX  -> malformed commands return OK            -> FAIL
POST-FIX -> malformed commands return ERR usage     -> PASS
"""
import sys
import time

sys.path.insert(0, sys.path[0])
from _harness import emulator                 # noqa: E402
from test_recontrol import ReControlClient    # noqa: E402

PORT = 6438

BAD = [
    "tap",            # missing args
    "tap banana",     # wrong count / non-numeric
    "tap 5 banana",   # non-numeric y (toInt -> 0 silently, pre-fix)
    "key",            # missing arg
    "key abc",        # non-numeric charcode
    "pen down 5",     # missing y
    "pen sideways 5 5",  # bad direction
    "button",         # missing args
]


def main():
    with emulator(PORT) as proc:
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"

        # The fix's effect: malformed input is REJECTED with ERR usage
        # (instead of the old swallowed OK).
        failures = []
        for cmd in BAD:
            r = (c.send_command(cmd) or "").strip()
            if not r.startswith("ERR usage"):
                failures.append((cmd, r))

        # No regression: well-formed input is still accepted.  Post-Phase-2 the
        # contract is honest — tap/pen/key/type return "OK delivered" (landmine
        # #3 closed); button keeps the queued "OK" (hardware-ISR path).  Ordered
        # list: "button power tap" is LAST because it sleeps the device, after
        # which anything queued would honestly be "ERR pending".
        good = [
            ("tap 80 80", "OK delivered"),
            ("tap 130 8", "OK delivered"),
            ("key 65", "OK delivered"),
            ("pen down 40 40", "OK delivered"),
            ("pen up 40 40", "OK delivered"),
            ("type hello", "OK delivered"),
            ("button power tap", "OK"),
        ]
        regressions = []
        for cmd, want in good:
            r = (c.send_command(cmd) or "").strip()
            if r != want:
                regressions.append((cmd, r))
        c.disconnect()

        if failures:
            lines = "\n".join(f"    {cmd!r} -> {resp!r}" for cmd, resp in failures)
            raise AssertionError(
                "ARG ERRORS SWALLOWED (expected 'ERR usage'):\n" + lines)
        if regressions:
            lines = "\n".join(f"    {cmd!r} -> {resp!r}" for cmd, resp in regressions)
            raise AssertionError("valid input rejected (regression):\n" + lines)
        print("PASS 1.8 — malformed WorkerDirect args return ERR usage; valid input accepted")


if __name__ == "__main__":
    main()
