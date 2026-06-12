#!/usr/bin/env python3
"""Phase 4 task 4.4: `info` must report the serial transport, and the live
PTY slave path once the guest has opened the port — the contract that lets
hosts (WildPalms) script HotSync end-to-end without scraping stderr.

  serial=<descriptor>             - always, when a serial transport is set
  serial=<descriptor> pty=<path>  - once the PTY exists (persists for the
                                    process after first open)
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6451
POSERRC = os.path.join(REPO, "build", ".poserrc")


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def main():
    backup = None
    if os.path.exists(POSERRC):
        backup = POSERRC + ".infoserial-backup"
        os.replace(POSERRC, backup)
    with open(POSERRC, "w") as f:
        f.write("PortSerial=serial:pty:HotSync\n")
    try:
        with emulator(PORT):
            c = connect(PORT, timeout=10)
            try:
                info = multiline(c, "info")
                assert "serial=serial:pty:HotSync" in info, \
                    f"info lacks serial descriptor:\n{info}"
                assert "pty=" not in info, \
                    f"pty= reported before the port ever opened:\n{info}"

                r = c.send_command("button cradle tap")
                assert r and r.startswith("OK"), f"cradle tap: {r!r}"

                deadline = time.time() + 20.0
                pty = None
                while time.time() < deadline:
                    info = multiline(c, "info")
                    m = re.search(r"pty=(/dev/pts/\d+)", info)
                    if m:
                        pty = m.group(1)
                        break
                    time.sleep(0.5)
                assert pty, f"pty= never appeared in info:\n{info}"
                assert os.path.exists(pty), f"reported pty does not exist: {pty}"
                print(f"PASS (pty={pty})")
            finally:
                c.disconnect()
    finally:
        if os.path.exists(POSERRC):
            os.unlink(POSERRC)
        if backup:
            os.replace(backup, POSERRC)


if __name__ == "__main__":
    main()
