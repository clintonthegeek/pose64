#!/usr/bin/env python3
"""Phase 4 task 4.4 + Phase 4.5 task 1: `info` must report the serial
transport descriptor and the live PTY slave path.

Phase 4.5 contract (eager PTY creation): when a pty: serial transport is
configured, the PTY is created at transport INSTALL (startup), not at the
guest's first port open — so `pty=/dev/pts/N` is present in `info` from the
moment the emulator answers, the path exists on disk, and HotSync tools can
attach BEFORE the first sync attempt.  The path is stable for the process
lifetime (OpenPtyPort reuses the master fd).
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6451


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def main():
    with emulator(PORT, extra_args=["-preference",
                                    "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
            # Eager creation: pty= reported from startup, before ANY guest
            # activity (short grace window for the install path to run).
            deadline = time.time() + 5.0
            pty = None
            info = ""
            while time.time() < deadline:
                info = multiline(c, "info")
                m = re.search(
                    r"serial=serial:pty:HotSync pty=(/dev/pts/\d+)", info)
                if m:
                    pty = m.group(1)
                    break
                time.sleep(0.25)
            assert pty, f"pty= not reported at startup (eager creation):\n{info}"
            assert os.path.exists(pty), f"reported pty does not exist: {pty}"

            # Stability: same path after the guest opens the port.
            r = c.send_command("button cradle tap")
            assert r and r.startswith("OK"), f"cradle tap: {r!r}"
            time.sleep(2.0)
            info2 = multiline(c, "info")
            assert f"pty={pty}" in info2, \
                f"pty path changed after guest open:\n{info2}"
            print(f"PASS (pty={pty}, stable across guest open)")
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
