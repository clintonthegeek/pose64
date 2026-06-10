#!/usr/bin/env python3
"""Repro 1.4: CPUWorkerThread::shutdown must be bounded so load/reset/exit
can't hang the main thread.

Mechanism (CPUWorkerThread.cpp): shutdown() queues CMD_SHUTDOWN and calls
QThread::wait() with no timeout (= forever).  If a slow or stuck handler is
in flight when shutdown() is called (from the main thread via RcCmd_Load),
the main thread blocks indefinitely — no TCP commands processed, no Qt events
dispatched.

After task 1.2, any EmSessionStopper inside a handler self-releases within its
own deadline (5000ms), so the original infinite-hang scenario no longer occurs
in practice.  The fix replaces the unbounded wait() with wait(8000) + terminate()
as a last-resort safety net, and sets fShouldStop = true before queueing the
sentinel to allow idle-worker fast-exit.

The fail scenario that existed BEFORE task 1.2 (kStopOnSysCall with no timeout,
blocking the worker thread indefinitely) is no longer reproducible here because
1.2 was committed first.  This repro instead verifies functional correctness of
the load path under concurrent command pressure, which would have failed
catastrophically before both 1.2 and 1.4.

Expected pass: load returns within deadline, new session is responsive.
"""
import sys
import time
import threading

sys.path.insert(0, sys.path[0])
from _harness import emulator              # noqa: E402
from test_recontrol import ReControlClient # noqa: E402

PORT = 6434
SPY_ADDR = "0x134"
LOAD_DEADLINE = 12.0   # load must complete within this many seconds


def wait_for(c, pred, timeout, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def force_blocked(c):
    """Drive the session to blocked_on_ui using the step spy."""
    if "blocked_on_ui" in (c.send_command("state") or ""):
        return
    resp = c.send_command("spy set " + SPY_ADDR) or ""
    if resp.startswith("ERR"):
        assert "blocked_on_ui" in (c.send_command("state") or ""), \
            f"spy set failed and not blocked_on_ui: {resp!r}"
        return
    assert wait_for(c, lambda: "blocked_on_ui" in (c.send_command("state") or ""), 5.0), \
        "setup: never reached blocked_on_ui"


def dialog_active(c):
    return "message=" in (c.send_command("dialog") or "")


def main():
    with emulator(PORT) as proc:
        # 1. Client A queues a kCmdWorkerSysCall while the CPU cannot reach
        #    a syscall boundary (blocked_on_ui).  After 1.2, this returns
        #    ERR timeout within 5000ms.  Without 1.2+1.4 together, the worker
        #    thread would block forever, and the subsequent load on client B
        #    would hang the main thread indefinitely.
        ca = ReControlClient(port=PORT, timeout=8)
        assert ca.connect(), "client A connect failed"
        force_blocked(ca)
        assert wait_for(ca, lambda: dialog_active(ca), 2.0), \
            "setup: dialog never appeared"

        # Queue a slow kCmdWorkerSysCall command on the worker.
        # apps is kCmdWorkerSysCall — blocked_on_ui prevents it from reaching
        # a syscall boundary so it will time out in ~5s (after 1.2 fix).
        # We launch this in a thread so it doesn't block us.
        slow_resp = []
        def run_slow():
            r = ca.send_command("apps")
            slow_resp.append(r)
        slow_thread = threading.Thread(target=run_slow, daemon=True)
        slow_thread.start()

        # Give the slow command time to be enqueued on the worker.
        time.sleep(0.2)

        # 2. Client B issues load while the slow command is in flight.
        #    This triggers gCPUWorker->shutdown() on the main thread, which
        #    calls QThread::wait().  Without the 1.4 fix, that wait() has no
        #    deadline; the main thread blocks for the full duration of the
        #    in-flight handler.
        cb = ReControlClient(port=PORT, timeout=LOAD_DEADLINE + 3)
        assert cb.connect(), "client B connect failed"

        t0 = time.time()
        load_resp = cb.send_command("load m515.psf")
        dt = time.time() - t0

        # 3. EFFECT: load must return within the deadline.
        assert load_resp.startswith("OK") or load_resp.startswith("ERR"), \
            f"load never responded: {load_resp!r}"
        assert dt < LOAD_DEADLINE, \
            f"HANG: load took {dt:.1f}s (max {LOAD_DEADLINE}s)"

        # 4. EFFECT: new session must be healthy after load.
        #    Disconnect both old clients (only one may be connected at a time)
        #    then reconnect fresh.
        ca.disconnect()
        cb.disconnect()
        slow_thread.join(timeout=2.0)  # drain the slow-command thread
        time.sleep(1.0)  # allow new session to settle
        cc = ReControlClient(port=PORT, timeout=8)
        assert cc.connect(), "post-load reconnect failed"
        state = cc.send_command("state") or ""
        assert state.startswith("OK"), \
            f"FAIL: new session not healthy after load: {state!r}"

        assert proc.poll() is None, "emulator process crashed"

        print(f"PASS 1.4 - load returned in {dt:.1f}s, new session healthy ({state.strip()})")
        cc.disconnect()


if __name__ == "__main__":
    main()
