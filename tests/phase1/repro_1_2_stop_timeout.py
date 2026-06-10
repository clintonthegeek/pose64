#!/usr/bin/env python3
"""Repro 1.2: kStopOnCycle commands must return a bounded, actionable error when
the CPU cannot reach a cycle boundary.

Mechanism (EmSession.cpp): SuspendThread() uses `useTimeout = (timeoutMs > 0 &&
how == kStopOnSysCall)`, so passing a timeout to a kStopOnCycle stopper has no
effect -- the wait loop calls fSharedCondition.wait() unconditionally.  The
kCmdWorkerCycle dispatch (ReControl.cpp) constructs the stopper with no timeout
at all, so a genuinely-nested CPU (IsNested()==true) can park the worker thread
forever -- every subsequent command hangs.

The kBlockedOnUI case (the one exercised here) exits the wait loop immediately
(1.1 fixed the counter leak), but currently returns the generic error:
  "ERR transient: could not stop session"
which gives no recovery guidance.

The 1.2 fix does two things:
1. SuspendThread: `useTimeout = (timeoutMs > 0)` -- drop the how==kStopOnSysCall
   gate so kStopOnCycle honours the timeout in the genuine-deadlock path.
2. kCmdWorkerCycle + kCmdAdaptive dispatch: pass 5000ms timeout and return
   "ERR timeout: CPU did not reach a cycle boundary within 5000ms.
    Recovery: dismiss any dialog (dialog respond) or palm_reset."

Expected fail (bug): resp starts with "ERR transient" (wrong, no recovery hint).
Expected pass (fix): resp starts with "ERR timeout" (actionable, bounded reply).
"""
import sys
import time

sys.path.insert(0, sys.path[0])
from _harness import emulator              # noqa: E402
from test_recontrol import ReControlClient # noqa: E402

PORT = 6433
SPY_ADDR = "0x134"
DEADLINE = 8.0   # kCmdWorkerCycle must reply within this many seconds


def wait_for(c, pred, timeout, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def force_blocked(c):
    """Drive the session to blocked_on_ui using the step spy."""
    if "blocked_on_ui" in (c.send_command("state") or ""):
        return
    resp = c.send_command("spy set " + SPY_ADDR) or ""
    if resp.startswith("ERR"):
        assert "blocked_on_ui" in (c.send_command("state") or ""), \
            f"spy set failed and not blocked_on_ui: {resp!r}"
        return
    assert wait_for(c, lambda: "blocked_on_ui" in (c.send_command("state") or ""), 5.0), \
        "setup: never reached blocked_on_ui"


def dialog_active(c):
    return "message=" in (c.send_command("dialog") or "")


def main():
    with emulator(PORT) as proc:
        c = ReControlClient(port=PORT, timeout=DEADLINE + 2)
        assert c.connect(), "connect failed"

        # 1. Get to blocked_on_ui and wait for the dialog to actually appear.
        force_blocked(c)
        assert wait_for(c, lambda: dialog_active(c), 2.0), \
            "setup: dialog never appeared after blocked_on_ui"

        # 2. Issue kCmdWorkerCycle while CPU is blocked on a dialog.
        #    kStopOnCycle cannot succeed (CPU is not at a cycle boundary) so
        #    the stopper returns false.
        #
        #    BUG: dispatch returns "ERR transient: could not stop session" --
        #    no guidance on recovery, and in the genuine-deadlock path (nested
        #    ROM call) the current code blocks forever.
        #
        #    FIX: dispatch passes a 5000ms timeout and returns:
        #      "ERR timeout: CPU did not reach a cycle boundary within 5000ms.
        #       Recovery: dismiss any dialog (dialog respond) or palm_reset."
        t0 = time.time()
        resp = c.send_command("ui") or ""
        dt = time.time() - t0

        # 3. EFFECT: bounded reply with actionable error.
        assert resp.startswith("ERR timeout"), \
            f"FAIL: expected 'ERR timeout', got {resp!r}"
        assert dt < DEADLINE, \
            f"FAIL: reply took {dt:.1f}s (must be < {DEADLINE}s)"

        # 4. EFFECT: control plane still answers after the failed stop.
        state = c.send_command("state") or ""
        assert state.startswith("OK"), \
            f"FAIL: control plane wedged after failed stop: {state!r}"

        # 5. Dismiss the dialog so the emulator can shut down cleanly.
        c.send_command("dialog respond continue")

        assert proc.poll() is None, "emulator process crashed"

        print("PASS 1.2 - kCmdWorkerCycle returns bounded ERR timeout with recovery hint")
        c.disconnect()


if __name__ == "__main__":
    main()
