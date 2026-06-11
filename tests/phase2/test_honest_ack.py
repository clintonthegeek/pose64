#!/usr/bin/env python3
"""Phase 2: the honest input contract (handoff §10 Q-ACK option a + task 2.4).

Exact-match on response strings — this test pins the new contract.
"""

import os
import sys
import time

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "tests"))
from phase1._harness import emulator  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from test_recontrol import ReControlClient  # noqa: E402

PORT = 6443


def main():
    with emulator(PORT):
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"
        try:
            run(c)
        finally:
            c.disconnect()
    print("PASS honest ACK contract")


def run(c):
    def send(cmd):
        return (c.send_command(cmd) or "").strip()

    # Delivered ACK on the awake path.
    assert send("tap 80 80") == "OK delivered", send("tap 80 80")
    assert send("key 65") == "OK delivered"
    assert send("type hi") == "OK delivered"

    # Pen events singly; duplicate pen-down is an honest error.
    assert send("pen down 50 50") == "OK delivered"
    r = send("pen down 50 50")
    assert r.startswith("ERR duplicate"), r
    assert send("pen up 50 50") == "OK delivered"

    # tap-id keeps coords, gains 'delivered'.
    # (1000 may not exist on the current form; only the prefix matters
    # when it does — accept the not-found error as well.)
    r = send("tap-id 1000")
    assert r.startswith("OK delivered") or r.startswith("ERR usage: object"), r

    # Drops are reported, not swallowed (task 2.4).
    assert send("gremlin new 1 1000000").startswith("OK")
    time.sleep(0.5)
    assert send("tap 80 80") == "ERR busy: gremlin running"
    assert send("key 65") == "ERR busy: gremlin running"
    assert send("gremlin stop").startswith("OK")
    time.sleep(0.5)
    assert send("tap 80 80") == "OK delivered"

    # Arg validation unchanged (task 1.8 regression guard).
    assert send("tap banana").startswith("ERR usage")

    # button keeps the queued contract (hardware-ISR path).
    assert send("button power tap") == "OK"


if __name__ == "__main__":
    main()
