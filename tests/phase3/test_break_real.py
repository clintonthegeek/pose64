#!/usr/bin/env python3
"""Landmine #1: a breakpoint hit with no SLP debugger must raise the
Continue/Debug/Reset dialog (blocked_on_ui), be inspectable, and resume on
'dialog respond continue'.  3x loop per repro convention.

Strategy for a deterministic hit address: backtrace frame PCs belong to the
active event loop — they re-execute on the next delivered event.  We set
breakpoint 0 at the current-frame PC (frame #0, always present — the live
stack-crawl depth varies between 1 and 2 frames, so pcs[1] is not reliable),
deliver a tap, and expect the hit.

Resume contract (deviation from the original plan draft, documented per the
plan's "adjust the frame strategy and note it" allowance): `break clearall`
is a WorkerCycle command and CANNOT run while the CPU is blocked_on_ui (it
needs a cycle boundary that never comes while parked on the dialog — this is
the same documented constraint repro_dialog_subsystem.py relies on). The
breakpoint PC is a hot event-loop address that re-hits immediately on resume,
so a single continue+clearall races the re-block. We therefore resume by
looping `dialog respond continue` (a Custom command, valid while blocked) and
retrying `break clearall` until it lands during a running window, then assert
the CPU is running.  The blocked_on_ui detection and dialog assertions are NOT
loosened.

Pre-fix: the guest never blocks (hit silently ignored) -> FAIL.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6427


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


def resume_and_clear(c, rnd):
    """Resume from the breakpoint dialog and clear all breakpoints.

    The breakpoint sits on a hot event-loop PC, and `break clearall` is a
    WorkerCycle command (it cannot run while blocked_on_ui — it needs a cycle
    boundary that never arrives while the CPU is parked on the dialog; this is
    the same documented constraint repro_dialog_subsystem.py relies on).  So
    each `dialog respond continue` resumes the CPU, which may re-reach the hot
    PC and re-block before `break clearall` lands.

    Robust resume sequence (validated by instrumentation): alternate
    `dialog respond continue` and `break clearall` without gating on a stale
    state read.  Once the CPU is running, `break clearall` reaches a cycle
    boundary and succeeds; with no breakpoint armed it then stays running.
    "no pending dialog" on continue is benign (the CPU is already running).
    Returns the final running state.
    """
    deadline = time.time() + 30.0
    while time.time() < deadline:
        r = c.send_command("dialog respond continue")
        if not r.startswith("OK") and "no pending dialog" not in r:
            raise AssertionError(f"round {rnd}: continue failed: {r}")
        r = c.send_command("break clearall")
        if r.startswith("OK"):
            # No breakpoint armed now; confirm it settles into running.
            final = wait_state(c, "running", timeout=3.0)
            if "running" in final:
                return final
        # else the CPU re-hit the bp before the cycle boundary; loop again.
    raise AssertionError(f"round {rnd}: break clearall never landed while running")


def one_round(c, rnd):
    bt = multiline(c, "backtrace")
    pcs = re.findall(r"PC=([0-9A-Fa-fx]+)", bt)
    assert len(pcs) >= 1, f"backtrace too shallow: {bt!r}"
    # Frame #0 is always present; deeper frames are not (stack-crawl depth
    # varies between deliveries).
    addr = pcs[0] if pcs[0].startswith("0x") else "0x" + pcs[0]

    r = c.send_command(f"break set 0 {addr}")
    assert r.startswith("OK"), f"break set failed: {r}"

    r = c.send_command("tap 80 80")  # drive the event loop through the frame
    assert r.startswith("OK") or "pending" in r, f"tap failed: {r}"

    state = wait_state(c, "blocked_on_ui")
    assert "blocked_on_ui" in state, f"round {rnd}: no dialog stop (state={state!r})"

    dlg = multiline(c, "dialog")
    assert "reakpoint" in dlg, f"round {rnd}: dialog is not the breakpoint dialog: {dlg!r}"
    assert addr.lower().replace("0x", "") in dlg.lower().replace("0x", ""), \
        f"round {rnd}: dialog lacks hit address {addr}: {dlg!r}"
    assert "regs" in dlg or "PC=" in dlg, f"round {rnd}: dialog lacks register dump: {dlg!r}"

    bt2 = multiline(c, "backtrace")  # inspectable while stopped
    assert "PC=" in bt2, f"round {rnd}: backtrace unavailable while blocked: {bt2!r}"

    state = resume_and_clear(c, rnd)
    assert "running" in state, f"round {rnd}: did not resume: {state!r}"
    print(f"PASS round {rnd} (hit @ {addr})")


def main():
    with emulator(PORT):
        c = connect(PORT, timeout=10)
        try:
            for rnd in (1, 2, 3):
                one_round(c, rnd)
                time.sleep(0.5)
        finally:
            c.disconnect()
    print("ALL PASS")


if __name__ == "__main__":
    main()
