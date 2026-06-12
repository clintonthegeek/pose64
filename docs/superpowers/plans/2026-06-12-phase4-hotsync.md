# Phase 4 — HotSync Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One documented, reproducible end-to-end HotSync between the emulated
Palm and pilot-link on the host — the project's reason to exist beyond parity
(recovery plan Phase 4; GATE 4: `pilot-xfer -l` lists the device's databases,
procedure reproducible from `docs/hotsync.md` by a fresh session).

**Architecture:** Experiment-first. Every mechanism already shipped (PTY
transport, scriptable cradle button, wall-clock 1x, accurate timers) but the
end-to-end test has never been run. Task 1 builds the reproduction harness
and runs the smoke test with ZERO emulator changes; only observed failures
unlock the (conditional) debug task. Productization (`info` exposes the PTY
path; `-preference` works same-run) lands TDD afterward so WildPalms can
script sync end-to-end.

**Tech stack:** Qt6/C++17 emulator (`build/pose64`), Python test harness
(`tests/lib/harness.py`), pilot-link (`/usr/bin/pilot-xfer`, package
`pilot-link-git` r1923; headers in `/usr/include/pi-*.h`).

---

## Verified mechanism map (read this first — all verified against live code 2026-06-12)

| Fact | Where |
|---|---|
| PTY is created in `EmTransportSerial::HostOpen` (posix_openpt), cached in `fHost->fPtyMaster`, prints `SERIAL: PTY created for "pty:HotSync" — connect HotSync tools to: /dev/pts/N` to **stderr** | `src/platform/EmTransportSerialUnix.cpp:600-647` |
| The PTY persists for the process once created ("Reusing existing PTY" on re-open) | `src/platform/EmTransportSerialUnix.cpp:600` |
| `EmTransportSerial::GetPtySlaveName()` accessor **already exists** | `src/platform/EmTransportSerialUnix.cpp:193-199` |
| Transport `Open()` happens when the guest raises the serial line driver (HotSync start) — NOT at process startup. STATUS.md's "prints at startup" was never verified; Task 1 observes reality | `src/core/PreferenceMgr.cpp:1966-1980` (`SetTransportForDevice` opens only `if (transport && gCPU && EmHAL::GetLineDriverState (type))`) |
| Serial-port pref key is literally `PortSerial`, an `EmTransportDescriptor`, default `null:` | `src/core/PreferenceMgr.h:304` |
| Correct descriptor is **`serial:pty:HotSync`** — schemes are `null:`/`serial:`/`socket:`/`usb:`; "pty:HotSync" is a serial *port name*, and a bare `pty:HotSync` string is mis-guessed as a socket by the Unix ctor heuristic | `src/core/EmTransport.cpp:396-426, 599-616` |
| Prefs file is `key=value` lines; `<exe-dir>/.poserrc` (i.e. `build/.poserrc`) **wins over** `~/.poserrc` if it exists; `POSER_DIR` env overrides exe-dir (but also moves skin lookup — don't use it) | `src/core/PreferenceMgr.cpp:1564-1594`, `src/platform/EmDirRefUnix.cpp:458+` |
| **Ordering gotcha:** `gPrefs->Load()` → `SetTransports()` runs BEFORE `-preference` CLI values are applied, so `-preference PortSerial=…` does NOT take effect in the same run today | `src/core/EmApplication.cpp:209` vs `src/core/Startup.cpp` (`PrvParseCommandLine` → `PrvHandlePreferenceParameters`) |
| `button cradle tap` → `kElement_CradleButton` → DragonBall `keyBitCradle` (m515 = VZ chip) → guest auto-starts a **local HotSync** | `src/core/ReControlCmds_Input.cpp:376`, `src/core/Hardware/EmRegsVZ.cpp:991,2591` |
| Serial logging: `log set Serial 2` and `log set SerialData 2`; `log dump` writes `Log_*.txt` into the emulator directory (`build/`) | `src/core/ReControlCmds_Debug.cpp:263-264,321-325`, `src/core/Logging.cpp` |
| m515 has **no** throttle-calibration entry (table holds only `PalmM500`) → m515 ticks are wall-true → the existing healthy `m515.psf` satisfies the spec's "uncalibrated device" intent. `Palm-Vx-4.0-en.rom` is on disk as fallback (`-rom Palm-Vx-4.0-en.rom -device PalmVx`, alias at `src/core/EmDevice.cpp:269-270`) | `src/core/EmDeviceBenchmark.h:35-43` |
| UART weak spot if the handshake stalls: RX pump cadence / FIFO overrun / RTS | `src/core/Hardware/EmUARTDragonball.cpp:630-647` (per recovery-plan 4.2) |
| `info` = `RcCmd_Info`, kCmdCustom: gathers config on the **main thread**, then formats in a worker lambda; `gEmuPrefs` extern via `PreferenceMgr.h:272` (already included); RTTI is on (no `-fno-rtti`) so `dynamic_cast` is fine | `src/core/ReControlCmds_Session.cpp:436+` |
| pilot-link source checkouts in `~/dev/WildPalms/` (`pilot-link`, `pilot-link-git`) are **broken self-referential symlinks** — for CMP/PADP/DLP internals use `/usr/include/pi-cmp.h` etc., or clone pilot-link fresh during Task 2 | verified 2026-06-12 |
| Test ports already in use: 6427-6429, 6431-6434, 6437/6438, 6440-6443, 6448, 6460. This plan uses **6450/6451/6452** | grep of `tests/` |

**Spec deviation, documented:** recovery-plan 4.1 step 1 says "Palm V/Vx ROM".
The intent (wall-true ticks, no m500-style 2.66× busy-tick skew) is satisfied
by the m515 — only `PalmM500` has a calibration entry. We use the existing
healthy, digitizer-calibrated `m515.psf` to avoid a new-session bring-up. If
(and only if) Task 2 evidence implicates device timing, create the Vx session:
`build/pose64 -rom Palm-Vx-4.0-en.rom -device PalmVx --port 6450`, calibrate
the digitizer by tapping the targets (`palm_screenshot` + `palm_tap`), save as
`vx.psf` (psf files are machine-local/gitignored).

---

### Task 1: HotSync smoke harness + first live run (spec 4.1 — "do this FIRST; it may just work")

**Files:**
- Create: `tests/phase4/__init__.py` (empty)
- Create: `tests/phase4/test_hotsync_smoke.py`

- [ ] **Step 1: Create the package marker**

```bash
touch tests/phase4/__init__.py
```

- [ ] **Step 2: Write the smoke script**

Create `tests/phase4/test_hotsync_smoke.py` with exactly:

```python
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
                t0 = time.time()
                retapped = False
                while proc.poll() is None:
                    if time.time() - t0 > PILOT_XFER_TIMEOUT:
                        proc.kill()
                        out, _ = proc.communicate()
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
```

- [ ] **Step 3: Verify line endings**

```bash
file tests/phase4/test_hotsync_smoke.py   # must NOT say CRLF
```

- [ ] **Step 4: Run the smoke test**

```bash
timeout 300 python3 tests/phase4/test_hotsync_smoke.py; echo "EXIT=$?"
```

Expected outcomes (record which one VERBATIM — numbers, log lines, pty path):
- **A. `HOTSYNC SMOKE PASS` (EXIT=0):** the milestone works. Run it twice
  more to confirm it is not a fluke. Skip Task 2 entirely; go to Task 3.
- **B. Any assert (EXIT=1):** the failure point + `/tmp/pose64_hotsync_smoke.log`
  + `build/Log_*.txt` + pilot-xfer output are the Task-2 evidence. Do NOT
  attempt fixes here (reproduce-first is done; root-causing is Task 2).

- [ ] **Step 5: Commit the harness (regardless of outcome) with an honest message**

```bash
git add tests/phase4/
# If outcome A:
git commit -m "test(phase4): HotSync smoke harness — END-TO-END PASS (pilot-xfer lists databases)"
# If outcome B (example; describe the actual observed failure):
git commit -m "test(phase4): HotSync smoke harness — reproduces <observed failure> (4.2 debug next)"
```

---

### Task 2 (CONDITIONAL — only if Task 1 outcome B): root-cause the handshake failure (spec 4.2)

**This task produces a diagnosis, not a pre-planned fix** (the bug is unknown
until observed). REQUIRED SUB-SKILL: superpowers:systematic-debugging. The
smoke script is the reproduction; re-run it after every change. Bound the
work: if 3+ fix attempts fail, stop and re-plan with the user (skill rule).

**Files (evidence in, findings out):**
- Read: `/tmp/pose64_hotsync_smoke.log` (stderr incl. PTY line + PRINTF serial traces)
- Read: `build/Log_*.txt` (the `log dump` output — Serial/SerialData level 2)
- Read: `src/core/Hardware/EmUARTDragonball.cpp:630-647` (RX pump — the spec's predicted weak spot), `src/platform/EmTransportSerialUnix.cpp` (comm threads)
- Create: `docs/superpowers/plans/2026-06-12-phase4-findings.md` (observed chain + root cause + fix plan)

- [ ] **Step 1: Localize the break in the chain** — the decision tree:

| Observation | Failing layer | Where to look next |
|---|---|---|
| No `SERIAL: PTY created` line ever | Guest never opened the serial port | `palm_screenshot` after cradle tap — did the HotSync app launch? Is the pref actually loaded (stderr `PRINTF` shows transport type at open; after Task 3, `info` shows `serial=`)? Was `.poserrc` seeding picked up (`build/.poserrc` existed during launch)? |
| PTY created, but `SerialData` log shows no TX bytes from guest | Guest→UART→transport TX path | `EmUARTDragonball` TX FIFO drain, line-driver/RTS state, transport comm threads (`CreateCommThreads`) |
| Guest CMP wakeup bytes visible, pilot-xfer attached, but no guest reaction to host reply | Host→guest RX path | RX pump cadence (`EmUARTDragonball.cpp:630-647` via `CycleSlowly`), FIFO overrun at 57600 baud; try `PILOTRATE=9600 pilot-xfer …` to remove rate pressure |
| CMP completes, DLP transfers start then time out | Throughput/timing | Confirm 1x throttle (`speed` query = 100); only if timing is implicated AND reproducible, escalate to spec 4.3 (wall-pace the timer accumulator) — that is a separate plan + user decision, NOT an inline fix |

- [ ] **Step 2: Protocol reference if needed** — pilot-link internals are NOT
  in `~/dev/WildPalms` (both checkouts are broken self-symlinks). Use
  `/usr/include/pi-cmp.h`, `/usr/include/pi-padp.h`, `/usr/include/pi-dlp.h`
  for packet formats, or clone pilot-link source fresh into `/tmp` for the
  serial state machine (`libpisock/serial.c`, `cmp.c`, `padp.c`).

- [ ] **Step 3: Write findings + fix plan** to
  `docs/superpowers/plans/2026-06-12-phase4-findings.md` (same format as the
  gate-3 findings doc: symptom / mechanism / fix, with log excerpts). Implement
  the fix ONLY with a failing-repro-first loop (the smoke script), one change
  at a time, and commit fix + findings doc together (R5).

- [ ] **Step 4: Re-run Task 1 Step 4 until outcome A, then proceed to Task 3.**

---

### Task 3: `info` reports the serial transport + live PTY path (spec 4.4, TDD)

WildPalms (and the smoke script itself) need a polling-friendly way to learn
the PTY slave path — today it exists only as a stderr line. `palm_state`
(MCP) already embeds the `info` response, so an `info` line is automatically
agent-visible.

**Files:**
- Create: `tests/phase4/test_info_serial.py`
- Modify: `src/core/ReControlCmds_Session.cpp` (RcCmd_Info, ~line 436)
- Modify: `docs/recontrol-protocol.md` (info command row)
- Modify: `claude/skills/palm-dev/SKILL.md` (HotSync workflow — written fully in Task 6)

- [ ] **Step 1: Write the failing test**

Create `tests/phase4/test_info_serial.py`:

```python
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
```

- [ ] **Step 2: Run it — must FAIL on the first assert** (no `serial=` line exists yet)

```bash
timeout 120 python3 tests/phase4/test_info_serial.py; echo "EXIT=$?"
```
Expected: `AssertionError: info lacks serial descriptor`, EXIT=1.

- [ ] **Step 3: Implement**

In `src/core/ReControlCmds_Session.cpp`:

(a) Add the include next to the existing ones at the top of the file:

```cpp
#include "EmTransportSerial.h"	// EmTransportSerial, GetPtySlaveName (Phase 4)
#include "Hardware/EmHAL.h"		// kUARTSerial
```

(b) In `RcCmd_Info`, in the main-thread gathering section (after the
`sessionPath` block, before `session->QueueWorkResult (...)`), add:

```cpp
	// Serial transport: configured descriptor + live PTY slave path
	// (Phase 4 — lets hosts script HotSync end-to-end; the PTY persists
	// for the process once the guest first opens the port).
	std::string serialInfo;
	{
		Preference<EmTransportDescriptor> pref (kPrefKeyPortSerial);
		std::string desc = pref->GetDescriptor ();
		if (!desc.empty () && desc != "null:")
		{
			serialInfo = desc;
			EmTransportSerial* serial = dynamic_cast<EmTransportSerial*> (
				gEmuPrefs->GetTransportForDevice (kUARTSerial));
			if (serial)
			{
				std::string pty = serial->GetPtySlaveName ();
				if (!pty.empty ())
					serialInfo += " pty=" + pty;
			}
		}
	}
```

(c) Add `serialInfo` to the worker lambda's capture list, and emit it in the
output block (next to the `os=`/`app=` lines):

```cpp
	if (!serialInfo.empty ())
		out += " serial=" + serialInfo + "\n";
```

Threading note for the reviewer: the gather runs on the main thread;
`GetTransportForDevice` reads `fTransports[]`, which is only written from the
main thread (`SetTransportForDevice`). `GetPtySlaveName` reads a string the
CPU thread writes once at first `HostOpen`; readers only consult it after the
PTY observably exists. If the TSAN sweep ever flags it, guard it with a small
mutex in `EmHostTransportSerial` — do not pre-add locking nobody measured.

- [ ] **Step 4: Build and run the test — must PASS**

```bash
make -C build -j$(($(nproc)-1)) pose64 2>&1 | tail -3
timeout 120 python3 tests/phase4/test_info_serial.py; echo "EXIT=$?"
```
Expected: `PASS (pty=/dev/pts/N)`, EXIT=0.

- [ ] **Step 5: Update the protocol doc**

In `docs/recontrol-protocol.md`, find the `info` command row and append to its
description: `Includes 'serial=<descriptor>[ pty=<path>]' when a serial
transport is configured; pty= appears once the guest opens the port.`

- [ ] **Step 6: Regression check + commit**

```bash
timeout 120 python3 tests/phase3/test_mcp_surface.py 2>&1 | tail -1
timeout 300 python3 tests/phase3/test_mcp_dispatch.py 2>&1 | tail -1
git add src/core/ReControlCmds_Session.cpp tests/phase4/test_info_serial.py docs/recontrol-protocol.md
git commit -m "feat(phase4): info reports serial transport + live PTY slave path"
```

---

### Task 4: make `-preference` effective in the same run (spec 4.4 enabler, TDD)

Today `-preference PortSerial=serial:pty:HotSync` writes the pref AFTER
`Load()` already created the transports, so it silently does nothing until
the NEXT run — a lying interface. Fix at the root: rebuild transports after
CLI prefs are applied.

**Files:**
- Create: `tests/phase4/test_preference_cli.py`
- Modify: `src/core/Startup.cpp` (`PrvParseCommandLine`, after the
  `PrvHandlePreferenceParameters` call — grep for it)

- [ ] **Step 1: Write the failing test**

Create `tests/phase4/test_preference_cli.py`:

```python
#!/usr/bin/env python3
"""Phase 4 task 4.4: '-preference PortSerial=serial:pty:HotSync' must take
effect in the SAME run. Pre-fix, EmApplication::Startup ran gPrefs->Load()
-> SetTransports() BEFORE the CLI prefs were applied, so the transport for
the run was built from the prefs-file value (typically null:) and the CLI
flag silently did nothing until the next run.

Effect-based: the assert is the PTY actually appearing after a cradle tap
(via the info pty= line from Task 3) — NOT the pref value, which lies
pre-fix (the preference IS updated; the transport is not).

Requires: no build/.poserrc seeding (that's the point)."""

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
    assert not os.path.exists(POSERRC), \
        f"{POSERRC} exists — remove it first, it would mask the CLI path"
    with emulator(PORT, extra_args=["-preference",
                                    "PortSerial=serial:pty:HotSync"]):
        c = connect(PORT, timeout=10)
        try:
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
            assert pty, ("-preference PortSerial did not take effect "
                         f"this run (no pty= in info):\n{info}")
            print(f"PASS (pty={pty})")
        finally:
            c.disconnect()


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it — must FAIL** (pty never appears; transport is null)

```bash
timeout 120 python3 tests/phase4/test_preference_cli.py; echo "EXIT=$?"
```
Expected: `AssertionError: -preference PortSerial did not take effect`, EXIT=1.

- [ ] **Step 3: Implement**

In `src/core/Startup.cpp`, inside `PrvParseCommandLine`, immediately after the
existing call that applies CLI preferences (`if (!Startup::
PrvHandlePreferenceParameters (prefs)) goto BadParameter;` or equivalent —
match the file's actual control flow), add:

```cpp
	// Command-line preferences are applied AFTER EmulatorPreferences::Load()
	// has already created the UART transports (EmApplication::Startup calls
	// gPrefs->Load() first), so a '-preference PortSerial=...' would
	// otherwise take effect only on the NEXT run.  Rebuild the transports
	// so the documented flag works in THIS run.  Safe with no session yet:
	// SetTransportForDevice's EmSessionStopper tolerates a null gSession
	// (same conditions as the Load()-time call).

	if (!prefs.empty ())
		gEmuPrefs->SetTransports ();
```

`gEmuPrefs` is declared in `PreferenceMgr.h` (line 272), which `Startup.cpp`
already includes (line 24).

- [ ] **Step 4: Build, run the test — must PASS; re-run Task 3's test (unchanged behavior)**

```bash
make -C build -j$(($(nproc)-1)) pose64 2>&1 | tail -3
timeout 120 python3 tests/phase4/test_preference_cli.py; echo "EXIT=$?"
timeout 120 python3 tests/phase4/test_info_serial.py; echo "EXIT=$?"
```
Expected: both `PASS`, EXIT=0.

- [ ] **Step 5: Commit**

```bash
git add src/core/Startup.cpp tests/phase4/test_preference_cli.py
git commit -m "fix(phase4): -preference takes effect same-run — rebuild transports after CLI prefs"
```

---

### Task 5: switch the smoke script to `-preference`, full re-verification

**Files:**
- Modify: `tests/phase4/test_hotsync_smoke.py`

- [ ] **Step 1: Replace the `.poserrc` seeding with the now-working CLI flag**

In `tests/phase4/test_hotsync_smoke.py`:
- Delete `seed_poserrc`, `restore_poserrc`, the `POSERRC` constant, the
  `backup = seed_poserrc()` / `restore_poserrc(backup)` wrapper, and the
  docstring paragraph beginning "Pref seeding: build/.poserrc".
- Change the emulator launch line to:

```python
        with emulator(PORT, capture_log=EMU_LOG,
                      extra_args=["-preference",
                                  "PortSerial=serial:pty:HotSync"]):
```

- Add to the docstring: `Pref seeding: '-preference PortSerial=serial:pty:HotSync'
  (works same-run since the Task-4 fix).`

- [ ] **Step 2: Run the smoke test 3× — all must PASS**

```bash
for i in 1 2 3; do timeout 300 python3 tests/phase4/test_hotsync_smoke.py || break; done
```
Expected: `HOTSYNC SMOKE PASS` three times.

- [ ] **Step 3: Regression sweep (the Phase-3 set + new phase-4 set)**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" >/dev/null 2>&1 && echo "PASS $r" || echo "FAIL $r"; done
python3 tests/phase2/test_honest_ack.py >/dev/null 2>&1 && echo "PASS honest_ack"
python3 tests/phase3/test_mcp_surface.py 2>&1 | tail -1
python3 tests/phase3/test_mcp_dispatch.py 2>&1 | tail -1
python3 tests/phase3/test_break_blocked_ops.py 2>&1 | tail -1
python3 tests/phase4/test_info_serial.py 2>&1 | tail -1
python3 tests/phase4/test_preference_cli.py 2>&1 | tail -1
```
Expected: everything PASS. Anything red blocks the commit.

- [ ] **Step 4: Commit**

```bash
git add tests/phase4/test_hotsync_smoke.py
git commit -m "test(phase4): smoke harness uses -preference (no .poserrc seeding)"
```

---

### Task 6: docs/hotsync.md + GATE 4 + Phase 4 close-out

**Files:**
- Create: `docs/hotsync.md`
- Modify: `claude/skills/palm-dev/SKILL.md` (add HotSync workflow section)
- Modify: `docs/STATUS.md` (HotSync section + Phase 4 record + header line)
- Modify: `docs/recovery-plan-2026-06.md` (CURRENT POSITION banner)

- [ ] **Step 1: Write `docs/hotsync.md`** — the verified procedure. Use this
content, replacing the two `<observed …>` fields with values from the actual
passing runs (this is data capture from Task 5, not design):

```markdown
# HotSync against pilot-link (verified <date of passing run>)

POSE64 syncs with pilot-link over a virtual PTY serial port. Verified
end-to-end on <date>: `pilot-xfer -l` lists the device's databases.
Automated reproduction: `python3 tests/phase4/test_hotsync_smoke.py`.

## Procedure (manual)

1. **Configure the serial transport.** Launch with the CLI flag (takes
   effect same-run):

       build/pose64 -psf m515.psf --port 6416 \
           -preference PortSerial=serial:pty:HotSync

   (Or set Preferences → Serial Port = `pty:HotSync` in the GUI once;
   it persists in `.poserrc`.)

2. **Start the sync from the guest.** Tap the cradle button — via MCP:
   `palm_button name=cradle action=tap` — or open the HotSync app and tap
   the Local sync icon. The guest opens its serial port, which creates the
   PTY. Observed timing: PTY appears ~<observed seconds>s after the tap.

3. **Find the PTY slave path.** Either of:
   - `palm_state` / ReControl `info` → ` serial=serial:pty:HotSync pty=/dev/pts/N`
   - stderr line: `SERIAL: PTY created for "pty:HotSync" — connect HotSync tools to: /dev/pts/N`

   The PTY persists for the emulator process once created.

4. **Run pilot-link against it.**

       pilot-xfer -p /dev/pts/N -l

   The Palm retries its connection attempt for a window of seconds; if the
   first attempt expired before pilot-xfer attached, tap the cradle again —
   with pilot-xfer already listening the second attempt connects race-free.

## Device choice

Use a device with NO throttle-calibration entry so PalmOS ticks stay
wall-true under load (`src/core/EmDeviceBenchmark.h` — only PalmM500 is
calibrated, with ~2.66× busy-tick skew corrected in the throttle, not the
timer). m515 and Palm Vx are both safe; the verified run used m515.

## Troubleshooting

- No PTY line after the cradle tap → the guest never opened the port:
  verify `info` shows `serial=serial:pty:HotSync`, and screenshot to
  confirm the HotSync app launched.
- Handshake stalls → enable `log set Serial 2` + `log set SerialData 2`,
  reproduce, `log dump` (writes `build/Log_*.txt`), and see
  `src/core/Hardware/EmUARTDragonball.cpp` RX pump. Slow the host side
  with `PILOTRATE=9600` to remove FIFO pressure.
- pilot-link protocol internals: `/usr/include/pi-{cmp,padp,dlp}.h`.
```

- [ ] **Step 2: Add the agent-facing workflow to SKILL.md** — new section after
the existing "Debugging Workflows" examples:

```markdown
### HotSync against pilot-link on the host

Launch the emulator with `-preference PortSerial=serial:pty:HotSync`, then:

```
palm_button name=cradle action=tap      # guest starts a local HotSync
palm_state                              # poll until info shows pty=/dev/pts/N
# host shell:  pilot-xfer -p /dev/pts/N -l
```

The PTY appears only after the first cradle tap (the guest opening its
serial port creates it) and then persists. If pilot-xfer attached too late
for the first attempt, tap the cradle again. Full procedure + troubleshooting:
`docs/hotsync.md`.
```

- [ ] **Step 3: Run the GATE 4 reproducibility check** — fresh agent, docs only.
Dispatch a `general-purpose` subagent with EXACTLY this prompt:

```
Following ONLY the procedure in docs/hotsync.md (read it first), perform a
HotSync between the POSE64 emulator and pilot-link on this host, starting
from a clean state (no emulator running). Use build/pose64 with m515.psf
on port 6420. Report: the exact commands you ran, the PTY path, the full
pilot-xfer database listing, and anything the doc got wrong or left out.
```

PASS = the agent produces a database listing without consulting anything but
docs/hotsync.md (+ the SKILL.md it references). Fix any doc gap it reports
and re-run the gate fresh.

- [ ] **Step 4: Update STATUS.md** —
  - Header date line: add `Phase 4 COMPLETE — GATE 4 PASSED <date>`.
  - Replace the "HotSync status" section: from "never been run" to the
    verified result (date, device, pilot-xfer listing summary, pointer to
    docs/hotsync.md + the smoke test).
  - Add the Phase 4 bullet under Recovery progress (smoke result, info
    serial line, -preference fix, gate evidence).
  - Add `docs/hotsync.md` to the authoritative document set table.

- [ ] **Step 5: Update the recovery-plan banner** — CURRENT POSITION: Phase 4
COMPLETE (GATE 4 PASSED <date>); NEXT ACTION: Phase 5 (declutter and ship
0.9.1 — dead-code deletions, small dedups, `main.cpp` shutdown fix, release).

- [ ] **Step 6: Final sweep, commit, tag, push**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" >/dev/null 2>&1 && echo "PASS $r" || echo "FAIL $r"; done
python3 tests/phase3/test_mcp_surface.py 2>&1 | tail -1
python3 tests/phase3/test_mcp_dispatch.py 2>&1 | tail -1
python3 tests/phase4/test_hotsync_smoke.py 2>&1 | tail -1
git add docs/hotsync.md claude/skills/palm-dev/SKILL.md docs/STATUS.md docs/recovery-plan-2026-06.md
git commit -m "docs(phase4): GATE 4 PASSED — HotSync verified end-to-end; next Phase 5"
git tag phase-4-complete
git push && git push --tags
```

---

## Explicitly out of scope (recovery plan "NOT doing" list still binds)

- Spec 4.3 (wall-pacing the timer accumulator) — only relevant if a
  *calibrated* device must sync; m515/Vx are uncalibrated. If Task 2
  evidence demands it anyway, that is a new plan + user decision.
- No UAE core changes, no multi-client ReControl, no new MCP tools beyond
  the `info` line, no Windows work.
- HotSync conduit behavior beyond `pilot-xfer -l` (database listing IS the
  GATE 4 criterion; full backup/install rounds are post-v1.0 polish).
