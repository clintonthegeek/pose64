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

ATTACH ORDER (Task-2 root cause, measured 2026-06-12, findings doc
docs/superpowers/plans/2026-06-12-phase4-findings.md):

The PTY only exists after the guest opens its serial port, i.e. AFTER the
first sync attempt has begun.  At uncalibrated emulated speed the guest's
CMP wakeup volley is tiny: wakeups every ~64ms for only ~1.2s after the
tap, port closed ~2.4s after the tap.  Attaching "right after the PTY
line" (0.2s log-poll + process spawn) usually lands AFTER that window —
it won the race only ~1 run in 5.

Two traps make a late attach fail instead of just waiting:
  a) The volley's ~18 wakeup packets queue in the slave's input buffer
     even while no slave is open.  A late pilot-xfer reads those STALE
     wakeups, believes a live device is present, completes CMP against
     the dead peer and moves to the DLP phase (observable: it sets the
     slave to the negotiated 230400 baud before any fresh tap).  The
     next real volley then looks like protocol garbage to it -> exits 1
     with 'Error read system info' (~2s, or ~0.3s after a fresh tap).
  b) After the failed attempt the guest shows the modal "HotSync
     Problem" form (id=12000); a cradle tap does NOT restart sync while
     it is up, so the old blind 15s re-tap could never recover (and
     pilot-xfer was long dead by then anyway).

The deterministic attach-then-tap order:
  1. tap cradle once -- sacrificial attempt; its only job is creating the
     PTY (which persists: fPtyMaster is cached for the process lifetime)
  2. wait for the "HotSync Problem" form -- positive proof the attempt is
     over and the guest serial port is closed
  3. dismiss it (tap-id 12004)
  4. flush the stale wakeups out of the PTY queue (trap a)
  5. attach pilot-xfer to the now-quiet PTY and give it time to open the
     port (the open slave also keeps the emulator's CommRead pump alive:
     reading a pty master returns EIO whenever no slave is open)
  6. tap cradle again -- the fresh wakeup volley goes straight to a raw,
     listening desktop at base rate; the handshake completes in ~1s

RESIDUAL ~10% PHASE RACE, so steps 2-6 retry up to MAX_ATTEMPTS times:
even in this order, pilot-xfer's CMP-init reply (host receipt <1ms after
the wakeup) is only delivered into the emulated UART on the RX pump's
~50ms quantum.  Measured: delivery 46ms after the wakeup -> the guest
accepts the init and switches to 230400; delivery 56ms -> the guest is
already past its per-wakeup listen window (64ms cycle), ignores the
identical bytes, exhausts its volley and pops the Problem form again
(pilot-xfer: 'Error accepting data').  Identical packet bytes in both
cases -- this is purely delivery-phase, so an independent retry of the
same procedure converges fast (~0.9 success per attempt).  Each failed
attempt ends in the same Problem form, so the loop precondition is
uniform.
"""

import os
import re
import subprocess
import sys
import termios
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6450
EMU_LOG = "/tmp/pose64_hotsync_smoke.log"
POSERRC = os.path.join(REPO, "build", ".poserrc")
PTY_RE = re.compile(r"connect HotSync tools to: (/dev/pts/\d+)")
PROBLEM_FORM_DEADLINE = 30.0   # a failed attempt shows the form within ~5s
PILOT_XFER_TIMEOUT = 60.0      # per-attempt wall budget for the listing
MAX_ATTEMPTS = 3               # ~0.9 per-attempt success (see docstring)


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


def wait_for_problem_form(c, deadline_s=PROBLEM_FORM_DEADLINE):
    """Poll the guest UI until the sacrificial attempt's failure form is up."""
    deadline = time.time() + deadline_s
    last_ui = ""
    while time.time() < deadline:
        last_ui = c.send_command("ui") or ""
        if "HotSync Problem" in last_ui:
            return True, last_ui
        time.sleep(0.5)
    return False, last_ui


def flush_pty(pty):
    """Discard stale bytes queued in the PTY (they survive the slave being
    closed -- the sacrificial volley would otherwise let pilot-xfer 'sync'
    with the dead first attempt and reject the real one)."""
    fd = os.open(pty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        termios.tcflush(fd, termios.TCIOFLUSH)
    finally:
        os.close(fd)


def sync_attempt(c, pty):
    """One deterministic attach-then-tap cycle (docstring steps 2-6).

    Precondition: a failed attempt has just ended (Problem form up or
    imminent). Returns (ok, pilot-xfer output)."""
    ok, ui = wait_for_problem_form(c)
    assert ok, (
        "guest never showed the 'HotSync Problem' form after a failed "
        f"attempt; last ui dump:\n{ui}")

    # Dismiss it, else cradle taps are swallowed.
    r = c.send_command("tap-id 12004")
    assert r and r.startswith("OK"), f"dismiss problem form: {r!r}"
    time.sleep(1.0)

    flush_pty(pty)

    # Attach pilot-xfer FIRST, to the now-quiet PTY. 1s is ample for it
    # to open the port.
    proc = subprocess.Popen(
        ["pilot-xfer", "-p", pty, "-l"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        time.sleep(1.0)
        if proc.poll() is None:
            # Re-arm the guest; the desktop is listening.
            r = c.send_command("button cradle tap")
            assert r and r.startswith("OK"), f"sync tap: {r!r}"

        try:
            out, _ = proc.communicate(timeout=PILOT_XFER_TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()
            try:
                out, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                out = "<pilot-xfer output unavailable>"
            return False, (f"pilot-xfer produced no listing within "
                           f"{PILOT_XFER_TIMEOUT:.0f}s:\n{out}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    if proc.returncode != 0:
        return False, f"pilot-xfer exit {proc.returncode}:\n{out}"
    return True, out


def main():
    backup = seed_poserrc()
    try:
        with emulator(PORT, capture_log=EMU_LOG):
            c = connect(PORT, timeout=10)
            try:
                # Serial observability. The Log* pref value is a BITMASK,
                # not a level: 1 = log during normal operation,
                # 2 = log ONLY while a Gremlin Horde runs, 3 = both
                # (EmTypes.h kNormalLogging/kGremlinLogging; gate is
                # LogCommon() in Logging.h). Value 2 outside a Horde logs
                # nothing -- that was the Task-2 "silent serial log".
                for cat in ("Serial", "SerialData"):
                    r = c.send_command(f"log set {cat} 1")
                    assert r and r.startswith("OK"), f"log set {cat}: {r!r}"

                # Step 1: sacrificial tap. Creates the (persistent) PTY.
                r = c.send_command("button cradle tap")
                assert r and r.startswith("OK"), f"cradle tap: {r!r}"

                pty = wait_for_pty()
                assert pty, (
                    "PTY never appeared in stderr after cradle tap — the "
                    f"guest never opened the serial port (see {EMU_LOG}; "
                    "check the HotSync app actually launched via screenshot)")
                print(f"PTY: {pty}")

                # Steps 2-6 with bounded retries: every failed attempt
                # (including the sacrificial one) ends in the Problem
                # form, so each cycle starts from the same state.
                ok, out = False, ""
                for attempt in range(1, MAX_ATTEMPTS + 1):
                    ok, out = sync_attempt(c, pty)
                    if ok:
                        break
                    print(f"attempt {attempt}/{MAX_ATTEMPTS} failed "
                          f"(delivery-phase race, see docstring):\n{out}")
                print("=== pilot-xfer output ===")
                print(out)
                assert ok, (
                    f"no successful sync in {MAX_ATTEMPTS} attempts; "
                    f"last failure:\n{out}")
                # Every Palm OS device carries the preferences databases.
                assert "Preferences" in out, f"no database listing:\n{out}"

                state = c.send_command("state") or ""
                assert "running" in state, f"post-sync state: {state!r}"
                print("HOTSYNC SMOKE PASS")
            except Exception:
                # Persist the serial log (now actually populated, see the
                # bitmask note above) for debugging. Writes build/Log_*.txt.
                try:
                    c.send_command("log dump")
                except Exception:
                    pass
                raise
            finally:
                c.disconnect()
    finally:
        restore_poserrc(backup)


if __name__ == "__main__":
    main()
