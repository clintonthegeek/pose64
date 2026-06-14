#!/usr/bin/env python3
"""Phase 4.5 task 2: the guest closing its serial port must not leave its
unanswered transmit data buffered in the pty.  A real serial line does not
store bytes for later listeners; pre-fix, the sacrificial CMP wakeup volley
(~18 x 26 bytes) queued in the slave input buffer and poisoned the next
pilot-xfer attach (`Error read system info` — see
docs/superpowers/plans/2026-06-12-phase4-findings.md).

Repro: tap the cradle with NOTHING attached, wait for the guest to give up
(the "HotSync Problem" form means the port is closed), then open the slave
and count readable bytes.  Contract: 0 (pre-fix: hundreds).
"""

import os
import re
import select
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6454


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def main():
    with emulator(PORT, extra_args=["-preference",
                                    "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
            # Eager PTY (Task 1): path available before any tap.
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

            r = c.send_command("button cradle tap")
            assert r and r.startswith("OK"), f"cradle tap: {r!r}"

            # The guest gives up ~2.4 s after the tap and shows the modal
            # "HotSync Problem" form (id=12000) — positive proof the port
            # has been closed again.
            deadline = time.time() + 30.0
            ui = ""
            while time.time() < deadline:
                ui = multiline(c, "ui")
                if "12000" in ui:
                    break
                time.sleep(0.5)
            assert "12000" in ui, f"guest never showed the Problem form:\n{ui}"

            # Count stale bytes a fresh listener would now read.
            fd = os.open(pty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                total = 0
                while True:
                    rd, _, _ = select.select([fd], [], [], 0.3)
                    if not rd:
                        break
                    data = os.read(fd, 4096)
                    if not data:
                        break
                    total += len(data)
            finally:
                os.close(fd)

            assert total == 0, (
                f"{total} stale bytes buffered in the pty after the guest "
                "closed the port — the close did not flush the volley")
            print("PASS (0 stale bytes after guest close)")
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
