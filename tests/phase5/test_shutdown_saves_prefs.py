#!/usr/bin/env python3
"""Phase 5: a NORMAL quit must save preferences (EmApplication::Shutdown ->
gPrefs->Save()).  Pre-fix, main.cpp's happy path returns before
theApp.Shutdown(), so prefs are not written on normal exit.

Effect test (R3): run quietly for a moment so any startup-time Save() settles,
snapshot the .poserrc state, send `quit` (the normal-exit path:
SetTimeToQuit -> QApplication::quit -> exec() returns -> Shutdown), wait for the
process to exit on its own, then assert .poserrc was (re)written by Shutdown.
"""

import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6458
POSERRC = os.path.join(REPO, "build", ".poserrc")


def state(path):
    try:
        return os.stat(path).st_mtime_ns
    except FileNotFoundError:
        return None


def main():
    with emulator(PORT) as proc:
        c = connect(PORT, timeout=10)
        try:
            time.sleep(2.0)  # let any startup-time Save() settle
            before = state(POSERRC)
            # Normal-exit path. quit is kCmdImmediate; the reply may or may not
            # arrive before the socket drops, so don't assert on it.
            try:
                c.send_command("quit")
            except Exception:
                pass
        finally:
            c.disconnect()
        # Let the process exit on its own (NOT killed by the harness).
        proc.wait(timeout=15)

    after = state(POSERRC)
    assert after is not None, (
        f".poserrc absent after a normal quit ({POSERRC}) — Shutdown did not run")
    assert before != after, (
        ".poserrc was not rewritten across the normal quit "
        f"(mtime {before} unchanged) — Shutdown did not save prefs on exit")
    print(f"PASS (.poserrc rewritten by Shutdown on normal quit: {before} -> {after})")


if __name__ == "__main__":
    main()
