#!/usr/bin/env python3
"""Phase 4 task 4.4: '-preference PortSerial=serial:pty:HotSync' must take
effect in the SAME run. Pre-fix, EmApplication::Startup ran gPrefs->Load()
-> SetTransports() BEFORE the CLI prefs were applied, so the transport for
the run was built from the prefs-file value (typically null:) and the CLI
flag silently did nothing until the next run.

Effect-based: the assert is the PTY actually appearing after a cradle tap
(via the info pty= line) -- NOT the pref value, which lies pre-fix (the
preference IS updated; the transport is not).

Precondition: build/.poserrc (if present) must not already configure the
pty transport -- otherwise the prefs-file path would mask the CLI path.
Deviation from the plan: the plan originally asserted not os.path.exists(POSERRC),
but a benign build/.poserrc with PortSerial=null: legitimately exists on this
machine. The precondition is therefore: if .poserrc exists, its PortSerial line
must be 'null:' or absent -- i.e. it must NOT already configure pty transport.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6452
POSERRC = os.path.join(REPO, "build", ".poserrc")


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def main():
    if os.path.exists(POSERRC):
        with open(POSERRC) as f:
            content = f.read()
        m = re.search(r"^PortSerial=(.*)$", content, re.MULTILINE)
        assert not (m and "pty" in m.group(1)), \
            f"{POSERRC} already configures the pty transport ({m.group(1)}) -- " \
            "it would mask the CLI path under test"

    with emulator(PORT, extra_args=["-preference",
                                    "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
            r = c.send_command("button cradle tap")
            assert r and r.startswith("OK"), f"cradle tap: {r!r}"

            deadline = time.time() + 20.0
            pty = None
            info = ""
            while time.time() < deadline:
                info = multiline(c, "info")
                m = re.search(r"pty=(/dev/pts/\d+)", info)
                if m:
                    pty = m.group(1)
                    break
                time.sleep(0.5)
            assert pty, ("-preference PortSerial did not take effect "
                         f"this run (no pty= in info):\n{info}")
            print(f"PASS (pty={pty})")
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
