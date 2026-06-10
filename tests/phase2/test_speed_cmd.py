#!/usr/bin/env python3
"""Phase 2: `speed` command — set/query emulation speed over ReControl."""

import os
import sys

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "tests"))

from phase1._harness import emulator  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from test_recontrol import ReControlClient  # noqa: E402

PORT = 6440


def main():
    with emulator(PORT):
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"
        try:
            checks = [
                ("speed max",    "OK"),
                ("speed",        "OK max"),
                ("speed 100",    "OK"),
                ("speed",        "OK 100"),
                ("speed 400",    "OK"),
                ("speed",        "OK 400"),
                ("speed banana", "ERR usage"),
                ("speed 0",      "ERR usage"),   # 0 must be spelled 'max'
                ("speed 100",    "OK"),          # restore 1x
            ]
            for cmd, want in checks:
                r = (c.send_command(cmd) or "").strip()
                assert r.startswith(want), f"{cmd!r} -> {r!r}, wanted prefix {want!r}"
        finally:
            c.disconnect()
    print("PASS speed command")


if __name__ == "__main__":
    main()
