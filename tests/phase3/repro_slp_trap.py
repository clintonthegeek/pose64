#!/usr/bin/env python3
"""Landmine #8 repro + regression: SLP debugger sockets (6414/2000).

Post-fix contract:
  1. Default launch: nothing listens on 6414/2000 (connection refused).
  2. --slp-debugger launch: 6414 accepts, and the control plane stays
     responsive through connect/disconnect (the EventCallback stoppers are
     bounded at 5000ms, so a connect can no longer wedge the UI thread
     indefinitely).

Pre-fix behavior (for the record, run once on the unfixed build): step 1
FAILS — the socket accepts by default.
"""

import os
import socket
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6427
SLP_PORTS = (6414, 2000)


def try_connect(port, timeout=2):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(("127.0.0.1", port))
        return s
    except OSError:
        s.close()
        return None


def assert_responsive(c, label, budget=2.0):
    t0 = time.time()
    r = c.send_command("state")
    dt = time.time() - t0
    assert r and r.startswith("OK") and dt < budget, f"{label}: {r!r} in {dt:.1f}s"


def main():
    # --- 1: off by default ---
    with emulator(PORT):
        for sp in SLP_PORTS:
            s = try_connect(sp)
            assert s is None, f"port {sp} accepted a connection with SLP off (default)"
            print(f"PASS default-off port {sp} refused")

    # --- 2: opt-in via --slp-debugger; control plane survives connect ---
    with emulator(PORT, extra_args=["--slp-debugger"]):
        c = connect(PORT)
        try:
            assert_responsive(c, "before SLP connect")
            s = try_connect(6414)
            assert s is not None, "port 6414 refused despite --slp-debugger"
            print("PASS opt-in port 6414 accepted")
            time.sleep(1.0)  # let kConnected (FtrSet stopper) run
            for i in range(6):  # > 5s bound: survives the worst-case stopper
                assert_responsive(c, f"during SLP connection (probe {i})", budget=7.0)
                time.sleep(1.0)
            s.close()
            time.sleep(1.0)  # let kDisconnected (FtrUnregister stopper) run
            assert_responsive(c, "after SLP disconnect", budget=7.0)
            print("PASS control plane responsive through connect/disconnect")
        finally:
            c.disconnect()
    print("ALL PASS")


if __name__ == "__main__":
    main()
