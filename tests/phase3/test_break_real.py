#!/usr/bin/env python3
"""Landmine #1: a breakpoint hit with no SLP debugger must raise the
Continue/Debug/Reset dialog (blocked_on_ui), be inspectable, and resume on
'dialog respond continue'.  3x loop per repro convention.

Strategy for a deterministic hit address: backtrace frame PCs belong to the
active event loop — they re-execute on the next delivered event.  We set
breakpoint 0 at the current-frame PC (frame #0, always present — the live
stack-crawl depth varies between 1 and 2 frames, so pcs[1] is not reliable),
deliver a tap, and expect the hit.

Resume contract (updated for the GATE 3 Gap 1 fix, 2026-06-12): `break` is
now an Adaptive command — it runs directly while the CPU is blocked_on_ui
(the CPU thread is frozen on the dialog, so the breakpoint table is safe to
modify).  The sane sequence is therefore: `break clearall` first (while
blocked), then `dialog respond continue` — with the table cleared there is
nothing to re-hit, even though the breakpoint PC is a hot event-loop address.
The dedicated repro for the blocked-clearall contract is
test_break_blocked_ops.py; this test uses the same sequence for resume.

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
    """Clear all breakpoints (while still blocked_on_ui — break is Adaptive
    since the GATE 3 Gap 1 fix), then resume.  With the table cleared there
    is nothing to re-hit, so a single continue settles into running.
    Returns the final running state.
    """
    r = c.send_command("break clearall")
    if not r.startswith("OK"):
        raise AssertionError(f"round {rnd}: break clearall while blocked failed: {r!r}")
    r = c.send_command("dialog respond continue")
    if not r.startswith("OK"):
        raise AssertionError(f"round {rnd}: continue failed: {r!r}")
    final = wait_state(c, "running", timeout=5.0)
    if "running" not in final:
        raise AssertionError(f"round {rnd}: did not settle into running: {final!r}")
    return final


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
