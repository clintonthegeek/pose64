#!/usr/bin/env python3
"""Gate 3 gap repro: breakpoint cleanup and load refusal while blocked_on_ui.

Gap 1 (gate-blocking): `break clearall` must work while the CPU is parked on
a breakpoint dialog (blocked_on_ui).  Pre-fix, `break` was a WorkerCycle
command: it waited for a CPU cycle boundary that never arrives while the CPU
is frozen on the dialog, and a breakpoint on a hot event-loop PC re-hits
before any boundary after every `dialog respond continue` — so breakpoint
cleanup via the control plane alone was impossible (`ERR timeout`), which
failed GATE 3 (see docs/superpowers/plans/2026-06-12-gate3-fail-findings.md).
Post-fix `break` is Adaptive (the same direct-when-blocked dispatch as
peek/poke/regs/backtrace): while blocked the CPU thread cannot touch the
breakpoint table, so the handler runs immediately on the main thread.

The sane sequence this enables (and the one the gate scenario needs):
    blocked_on_ui -> break clearall (OK) -> dialog respond continue -> running
with NO re-hit, no race loop, no emulator restart.

Gap 3 (quality): `load` while blocked_on_ui must refuse with an actionable
error instead of deadlocking.  Pre-fix it dismissed the dialog and deferred
teardown; with a hot breakpoint armed the CPU re-blocked before teardown ran,
and HandleClose then waited forever on a CPU thread parked on a dialog the
(stuck) main thread could never service.  Process kill was the only recovery.

3x rounds per repro convention; the load-refusal check runs in round 1 only
(it needs a saved psf and one blocked window).
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6431
SAVED_PSF = "/tmp/pose64_blocked_ops_round1.psf"


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


def one_round(c, rnd, check_load):
    # --- Arm a breakpoint on a hot event-loop PC (frame #0 re-executes on
    # the next delivered event) and drive execution through it.
    bt = multiline(c, "backtrace")
    pcs = re.findall(r"PC=([0-9A-Fa-fx]+)", bt)
    assert len(pcs) >= 1, f"round {rnd}: backtrace too shallow: {bt!r}"
    addr = pcs[0] if pcs[0].startswith("0x") else "0x" + pcs[0]

    r = c.send_command(f"break set 0 {addr}")
    assert r.startswith("OK"), f"round {rnd}: break set failed: {r}"

    r = c.send_command("tap 80 80")
    assert r.startswith("OK") or "pending" in r, f"round {rnd}: tap failed: {r}"

    state = wait_state(c, "blocked_on_ui")
    assert "blocked_on_ui" in state, f"round {rnd}: no dialog stop (state={state!r})"

    dlg = multiline(c, "dialog")
    assert "reakpoint" in dlg, f"round {rnd}: not the breakpoint dialog: {dlg!r}"

    # --- Gap 1: breakpoint cleanup MUST work while blocked_on_ui.
    # Pre-fix this returned "ERR timeout: CPU did not reach a cycle boundary".
    r = c.send_command("break clearall")
    assert r is not None and r.startswith("OK"), \
        f"round {rnd}: break clearall while blocked_on_ui failed: {r!r}"

    lst = multiline(c, "break list")
    assert "enabled" not in lst.replace("disabled", ""), \
        f"round {rnd}: slots still enabled after clearall: {lst!r}"

    # --- Gap 3 (round 1 only): load while blocked must refuse, not deadlock.
    if check_load:
        t0 = time.time()
        r = c.send_command(f"load {SAVED_PSF}")
        dt = time.time() - t0
        assert r is not None and r.startswith("ERR blocked"), \
            f"round {rnd}: load while blocked_on_ui: want ERR blocked, got {r!r} after {dt:.1f}s"
        assert dt < 3.0, f"round {rnd}: load refusal took {dt:.1f}s (must be immediate)"
        # Still blocked, dialog still pending — the refusal must not disturb it.
        state = c.send_command("state") or ""
        assert "blocked_on_ui" in state, \
            f"round {rnd}: load refusal disturbed the blocked state: {state!r}"

    # --- Resume: with the table cleared there is nothing to re-hit.
    r = c.send_command("dialog respond continue")
    assert r.startswith("OK"), f"round {rnd}: continue failed: {r}"

    state = wait_state(c, "running", timeout=5.0)
    assert "running" in state, f"round {rnd}: did not resume: {state!r}"

    # No re-hit: state must STAY running (the pre-fix failure mode was an
    # immediate re-block on the hot PC).
    for _ in range(6):
        time.sleep(0.25)
        state = c.send_command("state") or ""
        assert "blocked_on_ui" not in state, \
            f"round {rnd}: re-hit after clearall+continue: {state!r}"

    print(f"PASS round {rnd} (hit @ {addr}{', load refused' if check_load else ''})")


def main():
    with emulator(PORT):
        c = connect(PORT, timeout=10)
        try:
            # Saved session file for the round-1 load-refusal check (load
            # validates file existence synchronously, before the state check).
            r = c.send_command(f"save {SAVED_PSF}")
            assert r and r.startswith("OK"), f"setup: save failed: {r!r}"

            for rnd in (1, 2, 3):
                one_round(c, rnd, check_load=(rnd == 1))
                time.sleep(0.5)
        finally:
            c.disconnect()
            if os.path.exists(SAVED_PSF):
                os.unlink(SAVED_PSF)
    print("ALL PASS")


if __name__ == "__main__":
    main()
