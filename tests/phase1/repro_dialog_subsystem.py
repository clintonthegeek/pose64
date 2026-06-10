#!/usr/bin/env python3
"""Repro — BlockOnDialog action-lifetime UAF (DISCOVERED 2026-06-10).

This is an OPEN BUG (currently FAILS).  It is a hard prerequisite for verifying
landmine #2 (task 1.1) and task 1.2, both of which require a working
`blocked_on_ui` dialog to exercise kStopOnCycle while blocked.

Two verified crash interleavings (see docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md):

  (A) RESET-WHILE-SHOWING: `reset` -> EmDlgQt_DismissIfPending rejects the
      QMessageBox; ForceReset breaks the CPU out of BlockOnDialog; the CPU
      unwinds while the main thread is still inside msgBox.exec(); when exec
      returns, Do writes fDlgResult -- a reference to the dead stack `result`
      -- corrupting the CPU thread's live stack.

  (B) RESET-WHILE-QUEUED: within the <=100ms idle-tick window between
      blocked_on_ui and the dialog appearing, `reset` breaks BlockOnDialog's
      wait; the CPU unwinds, freeing the stack-local RunDialogParameters and
      EditCommonDialogData; the main thread later runs EmActionDialog::Do ->
      QString::fromUtf8(dangling fMessage) -> SIGSEGV.

Note: the earlier "dialog never shows" hang (Bug A) did NOT reproduce on
2026-06-10. Dialogs show within one idle tick (~100ms after blocked_on_ui).
A regression guard for the show latency is included as Part 1.

Verified on both QT_QPA_PLATFORM=offscreen and xcb (not a headless quirk).

Effect asserted: raising a deferred-error dialog then dismissing via `reset`
must leave the emulator ALIVE and responsive, in both interleavings.
"""
import sys
import time

sys.path.insert(0, sys.path[0])
from _harness import emulator                 # noqa: E402
from test_recontrol import ReControlClient    # noqa: E402

PORT = 6460
SPY_ADDR = "0x134"          # low-mem global written ~100x/s
SHOW_DEADLINE = 2.0         # dialog must show within this after blocked_on_ui


def wait_for(c, pred, timeout, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def get_blocked(c):
    # If already blocked_on_ui (spy armed from a prior call), no need to re-arm.
    if "blocked_on_ui" in (c.send_command("state") or ""):
        return
    resp = c.send_command("spy set " + SPY_ADDR) or ""
    if resp.startswith("ERR"):
        # WorkerCycle dispatch fails when session is blocked_on_ui; check if
        # we raced into that state between the state check and spy set.
        assert "blocked_on_ui" in (c.send_command("state") or ""), \
            f"spy set failed and not blocked_on_ui: {resp!r}"
        return
    assert wait_for(c, lambda: "blocked_on_ui" in (c.send_command("state") or ""), 5.0), \
        "setup: never reached blocked_on_ui"


def dialog_active(c):
    return "message=" in (c.send_command("dialog") or "")


def assert_alive_and_running(proc, port, label):
    assert proc.poll() is None, f"{label}: emulator crashed (signal/exit {proc.poll()})"
    c = ReControlClient(port=port, timeout=5)
    assert c.connect(), f"{label}: emulator unresponsive (connect failed)"
    # Accept OK running or OK blocked_on_ui: after a soft reset the spy
    # persists and can fire immediately, so the session may go straight back
    # into blocked_on_ui.  Either state proves the reset completed and the
    # emulator is healthy.
    def is_healthy():
        state = c.send_command("state") or ""
        return state.startswith("OK running") or state.startswith("OK blocked_on_ui")
    ok = wait_for(c, is_healthy, 20.0, 0.5)
    assert ok, f"{label}: never returned to OK running/blocked_on_ui after reset"
    c.disconnect()


def main():
    with emulator(PORT) as proc:
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"

        # --- Part 1: the dialog must actually show (hang regression guard) ---
        get_blocked(c)
        assert wait_for(c, lambda: dialog_active(c), SHOW_DEADLINE), \
            "HANG: dialog never became visible after blocked_on_ui"

        # --- Part 2: reset while the dialog is SHOWING (UAF write path) ---
        c.send_command("reset")
        c.disconnect()
        time.sleep(2.0)
        assert_alive_and_running(proc, PORT, "reset-while-showing")

        # --- Part 3: reset while the action is QUEUED (UAF read path).
        # Send reset as fast as possible after blocked_on_ui, repeatedly, to
        # land inside the <=100ms idle-tick window at least once.
        for i in range(4):
            c = ReControlClient(port=PORT, timeout=5)
            assert c.connect(), f"iter {i}: connect failed"
            get_blocked(c)
            c.send_command("reset")        # no dialog poll: race the idle tick
            c.disconnect()
            time.sleep(2.0)
            assert_alive_and_running(proc, PORT, f"reset-while-queued[{i}]")

        print("PASS - deferred-error dialogs survive reset in both interleavings")


if __name__ == "__main__":
    main()
