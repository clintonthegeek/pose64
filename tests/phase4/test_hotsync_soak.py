#!/usr/bin/env python3
"""Phase 4.5 task 3: ten consecutive normal-order HotSyncs (attach
pilot-xfer, tap once, expect the listing — NO retries) against one emulator
process.

Pre-fix this flakes: RX delivery rides CycleSlowly's 32K-instruction
(~50 ms) quantum against the guest's 64 ms per-wakeup CMP listen window —
measured ~10% per-attempt loss (findings doc: 46 ms delivered = win, 56 ms
= lose, byte-identical traffic).  Post-fix (event-driven pump) the contract
is 10/10.

Not part of the quick sweep (runtime ~2-3 min); run for serial-path changes.
"""

import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6455
ROUNDS = 10


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def dismiss_problem_form_if_up(c):
    ui = multiline(c, "ui")
    if "12000" in ui:
        c.send_command("tap-id 12004")
        time.sleep(1.0)


def one_round(c, pty, rnd):
    proc = subprocess.Popen(["pilot-xfer", "-p", pty, "-l"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(1.0)  # let it open the slave
        if proc.poll() is not None:
            out, _ = proc.communicate()
            return False, f"pilot-xfer died before the tap: {out}"
        r = c.send_command("button cradle tap")
        assert r and r.startswith("OK"), f"round {rnd} tap: {r!r}"
        try:
            out, _ = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, _ = proc.communicate()
            return False, f"round {rnd}: pilot-xfer hung:\n{out}"
        if proc.returncode == 0 and "Preferences" in out:
            return True, out
        return False, f"round {rnd}: exit {proc.returncode}:\n{out}"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def main():
    with emulator(PORT, extra_args=["-preference",
                                    "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
            deadline = time.time() + 5.0
            pty = None
            while time.time() < deadline:
                info = multiline(c, "info")
                m = re.search(r"pty=(/dev/pts/\d+)", info)
                if m:
                    pty = m.group(1)
                    break
                time.sleep(0.25)
            assert pty, f"no pty in info:\n{info}"

            failures = []
            for rnd in range(1, ROUNDS + 1):
                dismiss_problem_form_if_up(c)
                ok, detail = one_round(c, pty, rnd)
                print(f"round {rnd}: {'PASS' if ok else 'FAIL'}")
                if not ok:
                    failures.append(detail)
                time.sleep(1.5)  # let the guest settle after the sync

            assert not failures, (
                f"{len(failures)}/{ROUNDS} rounds failed:\n" +
                "\n---\n".join(failures))
            print(f"ALL PASS ({ROUNDS}/{ROUNDS} single-attempt syncs)")
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
