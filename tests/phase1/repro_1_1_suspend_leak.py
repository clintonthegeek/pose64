#!/usr/bin/env python3
"""Repro 1.1: kStopOnCycle command while a dialog is up leaks fSuspendByUIThread,
parking the CPU forever after the dialog is dismissed.

Mechanism (EmSession.cpp): SuspendThread increments fSuspendByUIThread
unconditionally for kStopNow/kStopOnCycle (lines 788/792), then the result
switch sets result=(fState==kSuspended) for kStopOnCycle (line 976); when the
CPU is kBlockedOnUI, result is false and the function returns without
decrementing -- the increment leaks.  The caller (EmSessionStopper) sees
Stopped()==false and does NOT call ResumeThread, so the counter stays +1.

After dialog dismiss, CheckForBreak sees fAllCounters != 0 and parks the CPU
in kSuspended.  The spy cannot re-fire from kSuspended (no 68K instructions
execute), so the CPU is permanently parked: state stays 'OK suspended:...'.

Expected pass: after dismiss, state is NOT suspended (CPU is healthy; spy
re-fires and CPU cycles normally between running and blocked_on_ui).
Expected fail (bug): state is 'OK suspended:...' (CPU permanently parked).

NOTE: the spy (0x134) re-fires almost immediately after dialog dismiss, so
the post-dismiss state is normally blocked_on_ui, not running.  The health
check is 'not suspended', not 'running'.
"""
import sys
import time

sys.path.insert(0, sys.path[0])
from _harness import emulator              # noqa: E402
from test_recontrol import ReControlClient # noqa: E402

PORT = 6432
SPY_ADDR = "0x134"
DIALOG_SHOW_DEADLINE = 2.0   # dialog must appear within this after blocked_on_ui


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
        c = ReControlClient(port=PORT, timeout=10)
        assert c.connect(), "connect failed"

        # 1. Get to blocked_on_ui (dialog queued, not yet visible)
        force_blocked(c)
        state = c.send_command("state") or ""
        assert "blocked_on_ui" in state, f"setup: not blocked_on_ui: {state!r}"

        # 2. Wait for the dialog to actually appear (~100ms idle tick latency).
        #    dialog respond fails silently if no dialog is showing.
        assert wait_for(c, lambda: dialog_active(c), DIALOG_SHOW_DEADLINE), \
            "setup: dialog never appeared after blocked_on_ui"

        # 3. Issue kCmdWorkerCycle while the dialog is showing.
        #    SuspendThread(kStopOnCycle) increments fSuspendByUIThread,
        #    sees kBlockedOnUI (not kSuspended), returns false — and without
        #    the fix, never decrements.  The counter leaks.
        leak_resp = c.send_command("ui")
        # (ERR transient is expected — kStopOnCycle cannot stop a blocked CPU)

        # 4. Dismiss the dialog so the CPU tries to resume.
        dismiss_resp = c.send_command("dialog respond continue")
        assert dismiss_resp.startswith("OK"), \
            f"dialog respond failed: {dismiss_resp!r}"

        # 5. Wait 2s for the CPU to settle.  With the bug, CheckForBreak sees
        #    fAllCounters != 0 (leaked) immediately after BlockOnDialog returns
        #    and parks the CPU in kSuspended — permanently, since no instructions
        #    run from kSuspended so the spy cannot re-fire.
        #    With the fix, the counter is balanced; the CPU resumes, the spy
        #    re-fires, and state cycles between running and blocked_on_ui.
        time.sleep(2.0)

        # 6. EFFECT: state must NOT be suspended.
        #    Suspended = CPU permanently parked by the leaked counter.
        #    blocked_on_ui = CPU healthy, spy re-fired (expected good state).
        state = c.send_command("state") or ""
        assert not state.startswith("OK suspended"), \
            f"LEAK: state={state!r} — CPU permanently parked after dialog dismiss"

        # 7. Sanity: emulator process still alive.
        assert proc.poll() is None, "emulator process crashed"

        print("PASS 1.1 - suspend-counter leak fixed (CPU not parked after dialog dismiss)")
        c.disconnect()


if __name__ == "__main__":
    main()
