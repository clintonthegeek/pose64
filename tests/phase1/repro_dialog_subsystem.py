#!/usr/bin/env python3
"""Repro — deferred-error dialog subsystem is broken (DISCOVERED 2026-06-10).

This is an OPEN BUG (currently FAILS).  It is a hard prerequisite for verifying
landmine #2 (task 1.1) and task 1.2, both of which require a working
`blocked_on_ui` dialog to exercise kStopOnCycle while blocked.

Two coupled defects (see docs/superpowers/findings/2026-06-10-dialog-subsystem.md):

  (A) HANG: when the CPU thread raises a deferred-error dialog (spy/watch/memory
      error), it posts an EmActionDialog and blocks in EmSession::BlockOnDialog.
      In this build the main thread never shows the dialog during normal idle —
      `dialog` reports `none` for many seconds and the CPU stays blocked_on_ui.

  (B) CRASH (UAF): `reset` breaks BlockOnDialog out of its wait via `fReset`
      (EmSession.cpp:1637) WITHOUT the dialog being answered; the CPU thread
      unwinds and frees the stack-local EditCommonDialogData, but the queued
      EmActionDialog still references it (fDlgParms / fDlgResult).  The main
      thread then runs EmActionDialog::Do -> RunDialog -> PrvHostCommonDialog ->
      QString::fromUtf8(dangling fMessage) -> SIGSEGV.

Verified on both QT_QPA_PLATFORM=offscreen and xcb (not a headless quirk).

Effect asserted: raising an error dialog then dismissing via `reset` must leave
the emulator ALIVE and responsive.  Today it crashes (SIGSEGV) or wedges.
"""
import sys
import time

sys.path.insert(0, sys.path[0])
from _harness import emulator                 # noqa: E402
from test_recontrol import ReControlClient    # noqa: E402

PORT = 6460
SPY_ADDR = "0x134"


def main():
    with emulator(PORT) as proc:
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"

        # Raise a deferred-error dialog (spy on a frequently-written global).
        assert (c.send_command("spy set " + SPY_ADDR) or "").startswith("OK")
        blocked = False
        for _ in range(40):
            if "blocked_on_ui" in (c.send_command("state") or ""):
                blocked = True
                break
            time.sleep(0.15)
        assert blocked, "setup: never reached blocked_on_ui"

        # Dismiss via reset (the documented unconditional recovery).
        try:
            c.send_command("reset")
        except Exception:
            pass
        time.sleep(1.5)

        if proc.poll() is not None:
            raise AssertionError(
                f"DIALOG SUBSYSTEM BUG: emulator crashed (signal/exit {proc.poll()}) "
                f"when reset dismissed a deferred-error dialog (BlockOnDialog UAF)")

        # If it survived, it must be responsive.
        try:
            c2 = ReControlClient(port=PORT, timeout=5)
            ok = c2.connect() and (c2.send_command("state") or "").startswith("OK")
            c2.disconnect()
        except Exception:
            ok = False
        if not ok:
            raise AssertionError("DIALOG SUBSYSTEM BUG: emulator unresponsive after reset")

        print("PASS — deferred-error dialog raised and dismissed without crash/hang")
        c.disconnect()


if __name__ == "__main__":
    main()
