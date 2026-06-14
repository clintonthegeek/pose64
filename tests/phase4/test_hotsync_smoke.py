#!/usr/bin/env python3
"""Phase 4 + 4.5: end-to-end HotSync smoke test against pilot-link, in the
NORMAL order (Phase 4.5 fixes made this the working order):

  launch with -preference PortSerial=serial:pty:HotSync
    -> PTY exists from startup (eager creation; pty= in `info`)
    -> attach pilot-xfer FIRST
    -> one cradle tap
    -> pilot-xfer lists the databases (GATE 4 criterion)

History: pre-4.5 this needed a sacrificial tap, a Problem-form dismissal,
and a pty flush (see docs/hotsync.md and the phase4 findings doc).  Those
fixes live in the emulator now (eager PTY, flush-on-close, event-driven RX
pump); a single bounded retry remains as a CI safety net and its use is
REPORTED — a retry firing regularly means a regression.
"""

import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6450
EMU_LOG = "/tmp/pose64_hotsync_smoke.log"
MAX_ATTEMPTS = 2


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def get_pty(c, deadline_s=5.0):
    deadline = time.time() + deadline_s
    info = ""
    while time.time() < deadline:
        info = multiline(c, "info")
        m = re.search(r"pty=(/dev/pts/\d+)", info)
        if m:
            return m.group(1)
        time.sleep(0.25)
    raise AssertionError(f"no pty= in info at startup:\n{info}")


def dismiss_problem_form_if_up(c):
    ui = multiline(c, "ui")
    if "12000" in ui:
        c.send_command("tap-id 12004")
        time.sleep(1.0)


def sync_attempt(c, pty):
    """Attach pilot-xfer, tap once, return (ok, output)."""
    proc = subprocess.Popen(["pilot-xfer", "-p", pty, "-l"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        time.sleep(1.0)  # let it open the slave
        if proc.poll() is not None:
            out, _ = proc.communicate()
            return False, f"pilot-xfer exited before the tap:\n{out}"
        r = c.send_command("button cradle tap")
        assert r and r.startswith("OK"), f"cradle tap: {r!r}"
        try:
            out, _ = proc.communicate(timeout=60)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                out, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                out = "<pilot-xfer output unavailable>"
            return False, f"pilot-xfer produced no listing in 60s:\n{out}"
        if proc.returncode == 0 and "Preferences" in out:
            return True, out
        return False, f"pilot-xfer exit {proc.returncode}:\n{out}"
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()


def main():
    with emulator(PORT, capture_log=EMU_LOG,
                  extra_args=["-preference",
                              "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
            pty = get_pty(c)
            print(f"PTY: {pty}")

            ok, out = False, ""
            for attempt in range(1, MAX_ATTEMPTS + 1):
                dismiss_problem_form_if_up(c)
                ok, out = sync_attempt(c, pty)
                if ok:
                    if attempt > 1:
                        print(f"NOTE: needed {attempt} attempts — "
                              "investigate if this recurs")
                    break
                print(f"attempt {attempt}/{MAX_ATTEMPTS} failed: {out}")

            print("=== pilot-xfer output ===")
            print(out)
            assert ok, f"all {MAX_ATTEMPTS} attempts failed; last:\n{out}"

            state = c.send_command("state") or ""
            assert "running" in state, f"post-sync state: {state!r}"
            print("HOTSYNC SMOKE PASS")
        except Exception:
            try:
                c.send_command("log dump")
            except Exception:
                pass
            raise
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
