#!/usr/bin/env python3
"""B2 code-review regression: clicking "Debug" on a breakpoint dialog with no
external SLP debugger attached must NOT re-enter the deferred-error queue.

Background (the bug this guards):
  When a breakpoint hit raises the Continue/Debug/Reset dialog, the CPU thread
  is parked inside EmSession::ExecuteSpecial's deferred-error iteration loop
  (the file-global gIterating == true).  `dialog respond debug` clicks Debug,
  which synchronously calls Debug::EnterDebugger(kException_SoftBreak, NULL).
  With no SLP debugger attached that returns non-errNone, so the throw is
  skipped and — before the fix — the no-debugger fallback ran
  ScheduleDeferredError(...) while gIterating == true.  ScheduleDeferredError
  begins with EmAssert(gIterating == false):
    - asserts-live build (POSE_DEBUG + POSE_ASSERTIONS): hard abort.
    - release build: appends to the std::list being iterated -> a redundant
      breakpoint dialog is queued.

The fix guards the fallback with EmSession::AreDeferredErrorsBeingHandled():
when a deferred-error dialog is already up the schedule is skipped and
HandleDialog's do/while simply re-shows the current dialog (matching the
watchpoint twin's Debug-button behavior).

This test runs against an asserts-live build (build="build-debug",
POSE_DEBUG=ON POSE_ASSERTIONS=ON) so the assert gives a crisp signal:
  - PRE-fix:  `dialog respond debug` aborts the process -> state stops
              round-tripping -> FAIL.
  - POST-fix: `dialog respond debug` is benign; the emulator stays alive,
              `state` round-trips, the dialog is still up (re-shown), and the
              CPU recovers to `running`.

Reproduction strategy mirrors test_break_real.py: breakpoint 0 on the
current-frame backtrace PC (a hot event-loop address), a tap to trigger.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6429
BUILD = "build-debug"  # asserts-live: POSE_DEBUG=ON POSE_ASSERTIONS=ON


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def wait_state(c, want, timeout=6.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = c.send_command("state") or ""
        if want in last:
            return last
        time.sleep(0.25)
    return last


def alive(c):
    """True if `state` round-trips (process still answering)."""
    r = c.send_command("state")
    return bool(r) and r.startswith("OK")


def trigger_breakpoint(c):
    """Arm bp 0 on the current-frame PC and tap to hit it. Returns the hit
    address. Asserts the breakpoint dialog comes up (blocked_on_ui)."""
    bt = multiline(c, "backtrace")
    pcs = re.findall(r"PC=([0-9A-Fa-fx]+)", bt)
    assert len(pcs) >= 1, f"backtrace too shallow: {bt!r}"
    addr = pcs[0] if pcs[0].startswith("0x") else "0x" + pcs[0]

    r = c.send_command(f"break set 0 {addr}")
    assert r.startswith("OK"), f"break set failed: {r}"

    r = c.send_command("tap 80 80")
    assert r.startswith("OK") or "pending" in r, f"tap failed: {r}"

    state = wait_state(c, "blocked_on_ui")
    assert "blocked_on_ui" in state, f"no breakpoint dialog stop (state={state!r})"

    dlg = multiline(c, "dialog")
    assert "reakpoint" in dlg, f"dialog is not the breakpoint dialog: {dlg!r}"
    return addr


def recover(c):
    """Resume from the breakpoint dialog and clear all breakpoints.

    Same constraint as test_break_real.resume_and_clear: `break clearall` is a
    WorkerCycle command and cannot land while blocked_on_ui; the bp sits on a
    hot PC that re-hits on resume.  Alternate continue + clearall until clearall
    lands during a running window.  Returns the final running state.
    """
    deadline = time.time() + 30.0
    while time.time() < deadline:
        r = c.send_command("dialog respond continue")
        if not r.startswith("OK") and "no pending dialog" not in r:
            raise AssertionError(f"continue failed: {r}")
        r = c.send_command("break clearall")
        if r.startswith("OK"):
            final = wait_state(c, "running", timeout=3.0)
            if "running" in final:
                return final
    raise AssertionError("break clearall never landed while running")


def main():
    with emulator(PORT, build=BUILD):
        c = connect(PORT, timeout=10)
        try:
            addr = trigger_breakpoint(c)
            print(f"breakpoint dialog up @ {addr}")

            # The re-entrancy trigger: click Debug with no SLP debugger.
            # Pre-fix this aborts the process; post-fix it is benign.
            r = c.send_command("dialog respond debug")
            assert r is not None, "process died on 'dialog respond debug' (no response)"
            assert r.startswith("OK") or "no pending dialog" in r, \
                f"unexpected 'dialog respond debug' reply: {r!r}"

            # Give any (buggy) re-scheduled error a moment to fire, then prove
            # the process is still alive and controllable.
            time.sleep(1.0)
            assert alive(c), "emulator stopped responding after 'dialog respond debug' (aborted?)"

            # A second Debug click must also be benign (re-entrancy is repeatable).
            r = c.send_command("dialog respond debug")
            assert r is not None, "process died on 2nd 'dialog respond debug'"
            time.sleep(0.5)
            assert alive(c), "emulator died after 2nd 'dialog respond debug'"

            # The dialog should still be up (re-shown, not dismissed by Debug).
            state = c.send_command("state") or ""
            assert "blocked_on_ui" in state, \
                f"dialog unexpectedly gone after Debug clicks (state={state!r})"

            # Recover to running — proves no zombie deferred-error queue.
            state = recover(c)
            assert "running" in state, f"did not recover to running: {state!r}"
            assert alive(c), "emulator not controllable after recovery"
            print(f"PASS (Debug click benign; recovered to running)")
        finally:
            c.disconnect()
    print("ALL PASS")


if __name__ == "__main__":
    main()
