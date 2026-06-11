#!/usr/bin/env python3
"""Integration test: input commands execute while the CPU worker runs.

Ported from the root-level scratch script onto tests/lib (Phase 3a, task 3.4).
"""

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402


def main():
    port = 6427
    with emulator(port):
        c = connect(port)
        try:
            for cmd in ("state", "tap 50 40", "key 65", "state"):
                resp = c.send_command(cmd)
                print(f"{cmd!r} -> {resp.strip() if resp else resp!r}")
                assert resp and resp.startswith("OK"), f"{cmd} failed: {resp}"
        finally:
            c.disconnect()
    print("PASS")


if __name__ == "__main__":
    main()
