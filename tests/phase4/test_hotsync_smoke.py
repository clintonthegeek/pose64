#!/usr/bin/env python3
"""Phase 4 task 4.1: end-to-end HotSync smoke test against pilot-link.

Mechanism chain under test (recovery plan Phase 4):
  seeded PortSerial pref (serial:pty:HotSync)
  -> 'button cradle tap' (DragonBall keyBitCradle) starts a local HotSync
  -> guest SerialMgr open raises the UART line driver
  -> EmTransportSerial::HostOpen creates the PTY and prints
     'SERIAL: PTY created for "pty:HotSync" - connect HotSync tools to: /dev/pts/N'
  -> pilot-xfer -p /dev/pts/N -l lists the device's databases (GATE 4)

Effect-based (R3): the verdict is pilot-xfer's database listing, not any
emulator response string.

Device: m515 session (m515.psf). m515 has NO throttle-calibration entry
(EmDeviceBenchmark.h holds only PalmM500), so its ticks are wall-true; this
satisfies the spec's "uncalibrated device" intent without a new-session
bring-up. Palm-Vx-4.0-en.rom is the on-disk fallback device (plan Task 2).

Pref seeding: build/.poserrc (binary-adjacent prefs win over ~/.poserrc,
Preferences::GetPrefRef). NOTE: '-preference PortSerial=...' does NOT work
same-run today: EmApplication::Startup runs gPrefs->Load() ->
SetTransports() BEFORE CLI prefs are applied. Plan Task 4 fixes that;
plan Task 5 switches this seeding over.

Known race, handled: the PTY only exists after the guest opens the serial
port, so the FIRST sync attempt starts before pilot-xfer can attach. The
Palm retries CMP wakeups for many seconds, so attaching right after the
PTY line appears normally wins. If pilot-xfer is still listening with no
result after RETAP_AFTER seconds, the guest's first attempt likely expired
before we attached -- the PTY persists (fPtyMaster is cached), so ONE
re-tap starts a second attempt with the desktop already listening
(race-free order).
"""

import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6450
EMU_LOG = "/tmp/pose64_hotsync_smoke.log"
POSERRC = os.path.join(REPO, "build", ".poserrc")
PTY_RE = re.compile(r"connect HotSync tools to: (/dev/pts/\d+)")
PILOT_XFER_TIMEOUT = 90.0   # total wall budget for the listing
RETAP_AFTER = 15.0          # re-arm the guest if no result by then


def seed_poserrc():
    """Point the binary-adjacent prefs at the PTY transport; keep a backup."""
    backup = None
    if os.path.exists(POSERRC):
        backup = POSERRC + ".smoke-backup"
        os.replace(POSERRC, backup)
    with open(POSERRC, "w") as f:
        f.write("PortSerial=serial:pty:HotSync\n")
    return backup


def restore_poserrc(backup):
    if os.path.exists(POSERRC):
        os.unlink(POSERRC)
    if backup:
        os.replace(backup, POSERRC)


def wait_for_pty(deadline_s=20.0):
    deadline = time.time() + deadline_s
    while time.time() < deadline:
        if os.path.exists(EMU_LOG):
            with open(EMU_LOG, errors="replace") as f:
                m = PTY_RE.search(f.read())
            if m:
                return m.group(1)
        time.sleep(0.2)
    return None


def main():
    backup = seed_poserrc()
    try:
        with emulator(PORT, capture_log=EMU_LOG):
            c = connect(PORT, timeout=10)
            try:
                # Serial observability (evidence for the Task-2 debug path).
                for cat in ("Serial", "SerialData"):
                    r = c.send_command(f"log set {cat} 2")
                    assert r and r.startswith("OK"), f"log set {cat}: {r!r}"

                r = c.send_command("button cradle tap")
                assert r and r.startswith("OK"), f"cradle tap: {r!r}"

                pty = wait_for_pty()
                assert pty, (
                    "PTY never appeared in stderr after cradle tap — the "
                    f"guest never opened the serial port (see {EMU_LOG}; "
                    "check the HotSync app actually launched via screenshot)")
                print(f"PTY: {pty}")

                proc = subprocess.Popen(
                    ["pilot-xfer", "-p", pty, "-l"],
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True)
                try:
                    t0 = time.time()
                    retapped = False
                    while proc.poll() is None:
                        if time.time() - t0 > PILOT_XFER_TIMEOUT:
                            proc.kill()
                            try:
                                out, _ = proc.communicate(timeout=5)
                            except subprocess.TimeoutExpired:
                                out = "<pilot-xfer output unavailable: communicate timed out>"
                            raise AssertionError(
                                f"pilot-xfer produced no listing within "
                                f"{PILOT_XFER_TIMEOUT:.0f}s:\n{out}")
                        if not retapped and time.time() - t0 > RETAP_AFTER:
                            # First guest attempt likely expired pre-attach;
                            # desktop is now listening, so re-arm once.
                            print("re-tapping cradle (attach-then-tap order)")
                            c.send_command("button cradle tap")
                            retapped = True
                        time.sleep(0.5)

                    out, _ = proc.communicate()
                    print("=== pilot-xfer output ===")
                    print(out)
                    assert proc.returncode == 0, \
                        f"pilot-xfer exit {proc.returncode}:\n{out}"
                    # Every Palm OS device carries the preferences databases.
                    assert "Preferences" in out, f"no database listing:\n{out}"
                finally:
                    if proc.poll() is None:
                        proc.kill()
                        proc.wait()

                state = c.send_command("state") or ""
                assert "running" in state, f"post-sync state: {state!r}"
                print("HOTSYNC SMOKE PASS")
            except Exception:
                # Persist the serial log for the Task-2 debug session.
                try:
                    c.send_command("log dump")  # writes build/Log_*.txt
                except Exception:
                    pass
                raise
            finally:
                c.disconnect()
    finally:
        restore_poserrc(backup)


if __name__ == "__main__":
    main()
