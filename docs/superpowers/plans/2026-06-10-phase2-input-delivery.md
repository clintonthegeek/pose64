# Phase 2 — Honest Input Delivery — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

> **⚠ PLAN REVISION — 2026-06-10 (execution session, HEAD `5cd5c62`). READ FIRST.**
> Task 1 is **done** (commit `5cd5c62`: `speed` command + a stale-comment fix).
> Then the R1 measurements overturned this plan's central test assumption — see
> the planning handoff **§11** for the full corrected ground-truth. Summary:
>
> 1. **Baseline delivery to an idle guest is 0%**, not a measurable nonzero
>    rate. The launcher sleeps in `evtWaitForever`; **no** posted input
>    (`tap`/`key`/`button`/`launch`) ever wakes it. The §11 probes ARE the R1
>    reproduction — **skip Task 2's separate "baseline characterisation" run**;
>    there is nothing to characterise but 0%.
> 2. **Task 2's setup is unworkable as written.** `button app1 tap` does NOT
>    enter Datebook on an idle guest (the "reliable hardware-ISR path" claim is
>    false at idle). There is no bootstrap-into-an-app on the baseline.
> 3. **Approach A cannot wake an already-asleep guest** (it lives only in the
>    `PuppetString` trap headpatch). So A is NOT a standalone mechanism and NOT
>    a fallback; **robust B (the STOP-exit `EvtWakeup` hook) is effectively
>    mandatory** and is *also* the only way to bootstrap any in-app test.
>
> **Revised execution order (replaces Tasks 2–5 sequencing):**
> - **R1 — Approach-B experiment FIRST** (was Task 4): implement the STOP-exit
>   hook on `phase2-experiment-B`; verify it makes the idle launcher deliverable
>   (idle launcher → `tap` Date Book icon → Datebook form appears). This is the
>   make-or-break gate AND the test bootstrap. **If B fails here, STOP and
>   escalate — there is no A-shaped rescue.**
> - **R2 — Delivery test** (revised Task 2): build/shake-out **on the B branch**,
>   targeting the idle→deliver effect (idle launcher → tap app icon → form
>   appears; restore via `key 264`/`launch Launcher`). Effect-based (R3).
>   Record baseline = 0% (from §11), and the B-branch delivery matrix.
> - **R3 — Approach-A idle cost** (revised Task 3): still measure A's 1x idle
>   cost, but only to answer the *demoted* question "is the awake-path
>   optimisation worth ~46%→~100% idle CPU?" A is no longer a delivery
>   candidate for the cold-asleep case.
> - **R4 — Checkpoint** (revised Task 5): the decision is **"B + C" vs "B + C +
>   A-style-no-sleep"**, not "A vs B". Cold-start data already argues for plain
>   **B + C**.
> - Tasks 6–9 (honesty plumbing, honest ACK contract, land-winner+delete-losers,
>   GATE 2) are **largely unchanged** — they assume B is the winner, which the
>   revision makes near-certain. GATE 2's delivery matrix runs the revised test.
>
> The task bodies below are kept for their detailed, still-valid steps (handler
> code, enum/counter design, deletion list). Where a task body conflicts with
> this banner, **the banner wins.**

**Goal:** `OK` from `tap`/`pen`/`key`/`type` means *delivered to the Palm OS
event queue* (or you get a truthful error), with exactly one wake mechanism in
the tree, passing GATE 2 (≥99% delivery over 200 taps at 1x and Max speed).

**Architecture:** Three layers. (1) *Measurement first*: a `speed` command, an
effect-based delivery test, and an idle-CPU harness characterize today's
failure rate and approach A's cost — the A-vs-B mechanism choice happens at a
data-backed checkpoint (Task 5), not before. (2) *Honesty layer (C, ships
regardless)*: `PostPenEvent`/`PostKeyEvent` return drop reasons; per-queue
delivery counters + a dedicated condvar let ReControl handlers block ≤2 s and
answer truthfully. (3) *Wake mechanism*: approach B is a STOP-exit `EvtWakeup`
hook on the CPU thread (verified feasible — handoff §10 Q-B3); approach A is
the preserved poll-always patch. Losers are deleted in the same commit as the
winner.

**Tech Stack:** C++17 (Qt6 main thread, omni_thread/raw-pthread CPU + worker
threads), Python 3 integration tests (self-launching, offscreen), TSAN/ASAN
builds in `build-tsan`/`build-asan`.

**Decisions this plan implements:** handoff §10 of
`docs/superpowers/plans/2026-06-10-phase2-planning-handoff.md` (Q-ACK = option
a, Q-DROP/Q-SYNC/Q-TEST/Q-IDLE/Q-SPEED/Q-DEV/Q-CLEAN as recorded there).

**Binding process rules (recovery plan R1–R6):** reproduce first; replace,
don't stack; assert effects, not responses; clean tree at session end; docs in
the same commit as the change they describe; no mega-sessions. **Natural
session break: after Task 5 (checkpoint).**

---

## File map

| File | Role in this plan |
|---|---|
| `src/core/ReControl.cpp` | command table, dispatch (validate in WorkerRaw), `speed` forward-decl |
| `src/core/ReControlCmds_Session.cpp` | new `RcCmd_Speed` |
| `src/core/ReControlCmds_Input.cpp` | tap/pen/key/type/tap-id/button handlers → honest ACK |
| `src/core/ReControl.h` | category comment updates |
| `src/core/EmSession.h` / `EmSession.cpp` | `EmPostInputResult`, delivery counters, wait/notify; delete `PrvWakeUpCPU` |
| `src/core/Patches/EmPatchMgr.cpp` | PuppetString delivery notifications (+ approach A edits, scratch branch only) |
| `src/core/Hardware/EmCPU68K.cpp` | approach B STOP-exit wake hook (experiment branch, then winner) |
| `src/core/CPUWorkerThread.h` | stale comment fix |
| `src/core/EmWindow.cpp` | stale comment fix |
| `tests/phase2/test_speed_cmd.py` | Task 1 test |
| `tests/phase2/test_delivery.py` | Task 2 — THE referee for everything |
| `tests/phase2/measure_idle_cpu.py` | Task 3 harness |
| `tests/phase2/test_honest_ack.py` | Task 6/7 test |
| `docs/recontrol-protocol.md`, `docs/architecture.md`, `docs/STATUS.md`, `docs/recovery-plan-2026-06.md` | updated in the same commits as the changes |

Build commands (existing trees): `cmake --build build -j$(nproc)`,
`cmake --build build-tsan -j$(nproc)`. Tests run the `build/` binary unless
stated. All test scripts are self-launching and offscreen
(`QT_QPA_PLATFORM=offscreen`), following `tests/phase1/_harness.py`.

---

### Task 1: `speed` ReControl command

GATE 2 requires runs at 1x **and Max**, and no speed surface exists —
`fEmulationSpeed` is GUI-menu-only (`EmApplication.cpp:1055`). Percent
semantics: `100` = 1x, `0` = Max (`EmSession.h:706`).

**Files:**
- Create: `tests/phase2/__init__.py` (empty), `tests/phase2/test_speed_cmd.py`
- Modify: `src/core/ReControl.cpp` (~line 47 forward decls, ~line 123 table)
- Modify: `src/core/ReControlCmds_Session.cpp` (new handler)
- Modify: `docs/recontrol-protocol.md` (Configuration section)

- [ ] **Step 1: Write the failing test**

`tests/phase2/__init__.py`: empty file.

`tests/phase2/test_speed_cmd.py`:

```python
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tests/phase2/test_speed_cmd.py`
Expected: `AssertionError: 'speed max' -> "ERR usage: unknown command 'speed'" ...`

- [ ] **Step 3: Implement the command**

`src/core/ReControlCmds_Session.cpp` — add `#include "PreferenceMgr.h"` to the
include block (after `"EmApplication.h"`), and add at the end of the file:

```cpp
// ============================================================================
// RcCmd_Speed — Immediate (main thread): set/query emulation speed.
// Percent semantics match EmApplication::DoSetSpeed: 100 = 1x, 0 = Max
// (spelled "max" on the wire so a bare 0 can't be sent by accident).
// ============================================================================

std::string RcCmd_Speed (const QStringList& args)
{
	if (!gSession)
		return "ERR transient: no session\n";

	if (args.size () == 1)
	{
		int speed = gSession->fEmulationSpeed.load (std::memory_order_relaxed);
		if (speed == 0)
			return "OK max\n";
		return "OK " + std::to_string (speed) + "\n";
	}

	if (args.size () != 2)
		return "ERR usage: speed [<percent>|max]\n";

	long speed;
	if (args[1].toLower () == "max")
	{
		speed = 0;
	}
	else
	{
		bool ok = false;
		speed = args[1].toLong (&ok);
		if (!ok || speed < 1 || speed > 10000)
			return "ERR usage: speed [<percent 1-10000>|max]\n";
	}

	Preference<long> p (kPrefKeyEmulationSpeed);
	p = speed;
	gSession->fEmulationSpeed.store ((int) speed, std::memory_order_relaxed);

	return "OK\n";
}
```

`src/core/ReControl.cpp` — add to the forward-declaration block (next to
`RcCmd_State`):

```cpp
std::string RcCmd_Speed (const QStringList& args);
```

and add a table row (in the configuration group of `sCommandTable`):

```cpp
	{"speed",          kCmdImmediate,     0,    RcCmd_Speed,      nullptr, nullptr},
```

- [ ] **Step 4: Build and run the test**

Run: `cmake --build build -j$(nproc) && python3 tests/phase2/test_speed_cmd.py`
Expected: `PASS speed command`

- [ ] **Step 5: Document (R5) and commit**

Add to `docs/recontrol-protocol.md` § Configuration table:

```markdown
| `speed [<percent>\|max]` | `OK\n` / `OK <percent>\n` / `OK max\n` | Set or query emulation speed (100 = 1x wall-clock, `max` = unthrottled). Needed for GATE 2 dual-speed runs. |
```

```bash
git add src/core/ReControl.cpp src/core/ReControlCmds_Session.cpp \
        tests/phase2/ docs/recontrol-protocol.md
git commit -m "feat(recontrol): speed command (set/query emulation speed)"
```

---

### Task 2: Delivery test (recovery-plan 2.1) + baseline characterization

> **⚠ REVISED — see top banner + handoff §11.** The setup below
> (`button app1 tap` → Datebook) does **not** work on the baseline (an idle
> guest never wakes). Build/shake-out this test **on the B branch** (after the
> approach-B experiment), and **retarget** it at the idle→deliver effect: idle
> launcher → `tap` an app icon (e.g. Date Book at ~`(20,48)`) → the app's form
> appears (`screen-hash`/`ui` change); restore via `key 264` (Home) or
> `launch Launcher`. The `find_button`/`wait_hash_change` helpers below are
> reusable as-is. Baseline = **0%** (recorded from §11) — skip Step 4's
> baseline matrix on the baseline binary; run the matrix on the B branch.

The referee for every later decision (R1/R3: effect-based — asserts on
`screen-hash` changes, never on response strings; it must stay valid across
the contract change). Target: Datebook "Go to" button opens the date-picker
dialog (screen change #1); its "Cancel" closes it (change #2). Datebook is
launched via the **hardware app button** (`button app1 tap`) — the
hardware-ISR path is reliable today and is NOT the path under test.

**Files:**
- Create: `tests/phase2/test_delivery.py`
- Modify: `docs/STATUS.md` (baseline numbers)

- [ ] **Step 1: Verify the target form interactively (R1 — don't trust assumptions)**

```bash
build/pose64 -psf m515.psf --port 6440 &   # with QT_QPA_PLATFORM=offscreen
sleep 8
printf 'button app1 tap\n' | timeout 5 nc -q1 localhost 6440
sleep 2
printf 'ui\n' | timeout 5 nc -q1 localhost 6440
printf 'quit\n' | timeout 5 nc -q1 localhost 6440
```

Expected: `OK FORM ... "Date Book"` (or similar title) containing a
`BUTTON id=NNNN "Go to..."` (label may vary slightly — note the exact label
and id). Then tap it once manually and dump `ui` again to confirm the dialog
shows a `BUTTON ... "Cancel"`. If the labels differ from `"Go"`/`"Cancel"`,
adjust the two `LABEL_*` constants in Step 2 — nothing else changes.

- [ ] **Step 2: Write the test**

`tests/phase2/test_delivery.py`:

```python
#!/usr/bin/env python3
"""Phase 2 delivery test (recovery-plan 2.1) — THE referee.

Effect-based (R3): a tap counts as delivered only if the screen hash changes
within TIMEOUT_S.  Asserts nothing about response strings (they change in
Phase 2); logs them for diagnostics.

Cycle: tap "Go to" (raw `tap` — the WorkerDirect path under test) -> date
picker opens -> tap "Cancel" (cached screen coords) -> day view returns.

Modes:
  rapid : taps back-to-back (awake-guest path)
  idle  : sleep IDLE_S before each "Go to" tap so the guest can reach STOP
          (exercises failure mode #1; answers handoff Q-B4)
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
    "tests"))
from phase1._harness import emulator  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)
from test_recontrol import ReControlClient  # noqa: E402

PORT = 6441
LABEL_GOTO = "Go"        # prefix match on the Datebook day-view button
LABEL_CANCEL = "Cancel"  # prefix match on the date-picker dialog button
TIMEOUT_S = 2.0
POLL_S = 0.05
IDLE_S = 1.0

BUTTON_RE = re.compile(r'BUTTON id=(\d+) "([^"]*)" \((\d+),(\d+),(\d+),(\d+)\)')


def send(c, cmd):
    return (c.send_command(cmd) or "").strip()


def screen_hash(c):
    r = send(c, "screen-hash")          # "OK <crc32> <w> <h>"
    parts = r.split()
    assert len(parts) >= 2 and parts[0] == "OK", f"screen-hash failed: {r!r}"
    return parts[1]


def read_ui(c):
    """`ui` is multi-line, '.'-terminated."""
    c.socket.sendall(b"ui\n")
    buf = b""
    deadline = time.time() + 5.0
    while time.time() < deadline:
        buf += c.socket.recv(65536)
        if b"\n.\n" in buf or buf.endswith(b".\n"):
            return buf.decode("utf-8", "replace")
    raise AssertionError("ui response did not terminate")


def find_button(ui_text, label_prefix):
    for m in BUTTON_RE.finditer(ui_text):
        oid, label, x, y, w, h = m.groups()
        if label.startswith(label_prefix):
            return int(oid), int(x) + int(w) // 2, int(y) + int(h) // 2
    raise AssertionError(f"no BUTTON starting {label_prefix!r} in:\n{ui_text}")


def wait_hash_change(c, h0, timeout=TIMEOUT_S):
    deadline = time.time() + timeout
    while time.time() < deadline:
        h = screen_hash(c)
        if h != h0:
            return h, True
        time.sleep(POLL_S)
    return h0, False


def wait_hash_equals(c, target, timeout=TIMEOUT_S):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if screen_hash(c) == target:
            return True
        time.sleep(POLL_S)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=["rapid", "idle"], default="rapid")
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--speed", default="100", help="percent or 'max'")
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    with emulator(args.port):
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        try:
            run(c, args)
        finally:
            c.disconnect()


def run(c, args):
    print(send(c, f"speed {args.speed}"))

    # --- setup: into Datebook via the hardware button (reliable ISR path)
    send(c, "button app1 tap")
    time.sleep(2.0)
    ui = read_ui(c)
    assert "Date" in ui.splitlines()[0], f"not in Datebook:\n{ui.splitlines()[0]}"
    _, gx, gy = find_button(ui, LABEL_GOTO)   # day view: form origin is 0
    h_main = screen_hash(c)

    # --- one discovery pass for Cancel's SCREEN coords: tap-id both taps
    # AND returns "OK <cx> <cy>" with window origin applied.
    send(c, f"tap {gx} {gy}")
    _, opened = wait_hash_change(c, h_main, timeout=5.0)
    assert opened, "setup: Go-to dialog never opened (baseline may be badly broken; rerun)"
    cancel_id, _, _ = find_button(read_ui(c), LABEL_CANCEL)
    r = send(c, f"tap-id {cancel_id}")        # closes the dialog, gives coords
    m = re.match(r"OK\D*(\d+) (\d+)", r)
    assert m, f"tap-id failed: {r!r}"
    cx, cy = int(m.group(1)), int(m.group(2))
    assert wait_hash_equals(c, h_main, timeout=5.0), "setup: dialog did not close"

    # --- measured loop
    delivered = 0
    failures = []
    latencies = []
    responses = {}
    for i in range(args.count):
        if args.mode == "idle":
            time.sleep(IDLE_S)
        h0 = screen_hash(c)
        t0 = time.time()
        resp = send(c, f"tap {gx} {gy}")
        responses[resp] = responses.get(resp, 0) + 1
        _, changed = wait_hash_change(c, h0)
        if changed:
            delivered += 1
            latencies.append(time.time() - t0)
        else:
            failures.append(i)
        # restore: close the dialog if it opened; recover if stuck
        send(c, f"tap {cx} {cy}")
        if not wait_hash_equals(c, h_main):
            send(c, f"tap {cx} {cy}")
            if not wait_hash_equals(c, h_main, timeout=5.0):
                print(f"ABORT: unrecoverable UI state at iteration {i}")
                break

    n = len(latencies)
    lat = sorted(latencies)
    print(f"mode={args.mode} speed={args.speed} count={args.count}")
    print(f"delivered={delivered}/{args.count} "
          f"({100.0 * delivered / max(1, args.count):.1f}%)")
    if n:
        print(f"latency p50={lat[n // 2] * 1000:.0f}ms "
              f"p95={lat[min(n - 1, int(n * 0.95))] * 1000:.0f}ms "
              f"max={lat[-1] * 1000:.0f}ms")
    print(f"failures at: {failures[:20]}")
    print(f"responses: {responses}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Shake out the test at small count**

Run: `python3 tests/phase2/test_delivery.py --mode rapid --count 5`
Expected: completes with `delivered=N/5` and a latency line. Fix label
constants/parsing if the setup assertions fire (use the Step 1 dump).

- [ ] **Step 4: Baseline characterization (the R1 reproduction)**

```bash
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed max
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed max
```

Expected: four `delivered=` lines. Record ALL four numbers + latency p50/p95.
**The idle-mode failure count is the empirical answer to handoff Q-B4.**

- [ ] **Step 5: Record + commit**

Add to `docs/STATUS.md` (Phase 2 bullet in "Recovery progress"): the four
baseline numbers, dated, marked "baseline at `<HEAD sha>`".

```bash
git add tests/phase2/test_delivery.py docs/STATUS.md
git commit -m "test(phase2): delivery test (2.1) + baseline failure-rate data"
```

---

### Task 3: Idle-CPU harness + baseline + approach-A cost (Q-IDLE)

**Files:**
- Create: `tests/phase2/measure_idle_cpu.py`
- Modify (scratch branch only): `src/core/Patches/EmPatchMgr.cpp`
- Modify: `docs/STATUS.md` (numbers)

- [ ] **Step 1: Write the harness** (no `pidstat` dependency — read `/proc` directly)

`tests/phase2/measure_idle_cpu.py`:

```python
#!/usr/bin/env python3
"""Phase 2 idle-CPU harness (Q-IDLE).

Boots m515.psf to wherever the session left off (launcher), applies a speed,
waits SETTLE_S with zero input, then samples the pose64 process's CPU% over
DURATION_S via /proc/<pid>/stat (utime+stime deltas).  Prints the mean.

Caveats recorded with the numbers (handoff §10 Q-IDLE): offscreen platform
(no real paint cost); Max idle pegs a core at baseline by design
(EmCPU68K.cpp STOP-loop sleep is speed>0-gated) — A's cost is only
meaningful at 1x.
"""

import argparse
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

PORT = 6442
CLK_TCK = os.sysconf("SC_CLK_TCK")


def proc_cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        fields = f.read().rsplit(") ", 1)[1].split()
    utime, stime = int(fields[11]), int(fields[12])  # fields 14,15 of stat
    return (utime + stime) / CLK_TCK


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--speed", default="100")
    ap.add_argument("--settle", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=60.0)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    with emulator(args.port) as proc:
        c = ReControlClient(port=args.port, timeout=5)
        assert c.connect(), "connect failed"
        print(c.send_command(f"speed {args.speed}"))
        c.disconnect()  # no open client during measurement — true idle

        time.sleep(args.settle)
        cpu0, t0 = proc_cpu_seconds(proc.pid), time.time()
        time.sleep(args.duration)
        cpu1, t1 = proc_cpu_seconds(proc.pid), time.time()

        pct = 100.0 * (cpu1 - cpu0) / (t1 - t0)
        print(f"idle CPU: {pct:.2f}%  (speed={args.speed}, "
              f"settle={args.settle}s, window={t1 - t0:.0f}s, offscreen)")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Baseline runs (3× each, record the median)**

```bash
for i in 1 2 3; do python3 tests/phase2/measure_idle_cpu.py --speed 100; done
for i in 1 2 3; do python3 tests/phase2/measure_idle_cpu.py --speed max; done
```

Expected: six `idle CPU: N.NN%` lines. The 1x baseline is the
"sleep-until-interrupt idle" headline number being protected.

- [ ] **Step 3: Approach-A cost on a scratch branch**

```bash
git checkout -b phase2-measure-A
```

Apply the clean A change (the 2026-03-13 patch minus its `fprintf`s) to
`src/core/Patches/EmPatchMgr.cpp`, function `PuppetString`:

Edit 1 — key branch: after `::StubAppEnqueueKey (event.fKey, 0, modifiers);`
(currently `EmPatchMgr.cpp:1056`), add:

```cpp
			// Approach A: skip SysEvGroupWait so it doesn't sleep — the key
			// is already enqueued and EvtGetEvent will find it.
			::PrvForceNilEvent ();
			callROM = kSkipROM;
			return;
```

Edit 2 — pen branch: after `StubAppEnqueuePt (&palmPen);` (currently
`:1071`), add:

```cpp
			// Approach A: skip SysEvGroupWait so it doesn't sleep — the event
			// is already enqueued and EvtGetEvent will find it.
			::PrvForceNilEvent ();
			callROM = kSkipROM;
			return;
```

Edit 3 — after the app-switch `else if` block closes (after
`clearTimeout = true;` / `}` at currently `:1084-1085`), add:

```cpp
		// Approach A: in interactive mode, never let SysEvGroupWait sleep
		// with an infinite timeout — the guest polls, so a pen/key event
		// posted while it would otherwise sleep in STOP is delivered on
		// the very next EvtGetEvent.
		if (!Hordes::IsOn () && !EmEventPlayback::ReplayingEvents ())
		{
			clearTimeout = true;
		}
```

```bash
cmake --build build -j$(nproc)
git add src/core/Patches/EmPatchMgr.cpp
git commit -m "experiment(phase2): approach A poll-always (measurement branch only)"
for i in 1 2 3; do python3 tests/phase2/measure_idle_cpu.py --speed 100; done
python3 tests/phase2/test_delivery.py --mode idle --count 100 --speed 100
git checkout master
cmake --build build -j$(nproc)   # restore baseline binary
```

Expected: A's 1x idle CPU% (3 runs) **and** A's idle-mode delivery rate (it
should be ~100% — if it isn't, that's checkpoint-relevant data too). Keep the
branch for the checkpoint; it is deleted in Task 8.

- [ ] **Step 4: Record + commit**

Append the baseline and approach-A numbers (with caveats from the harness
docstring) to the `docs/STATUS.md` Phase 2 bullet.

```bash
git add tests/phase2/measure_idle_cpu.py docs/STATUS.md
git commit -m "test(phase2): idle-CPU harness + baseline and approach-A idle cost"
```

---

### Task 4: Approach-B experiment (STOP-exit `EvtWakeup` hook)

> **⚠ REVISED — this runs FIRST (it is the bootstrap + make-or-break), and its
> primary pass criterion is the §11 cold-asleep case:** on `phase2-experiment-B`,
> from a fresh idle launcher, `tap`-ing the Date Book icon must actually open
> Datebook (`ui`/`screen-hash` change) within the timeout — i.e. B delivers to a
> guest that was asleep in `evtWaitForever`. Only after that passes is the
> revised delivery test (Task 2) built on this branch. **If this fails, STOP and
> escalate** — approach A cannot rescue the cold-asleep case (handoff §11.3).

The handoff §10 Q-B3 design, on a scratch branch. Pass criteria: (a) all four
delivery-test runs ≥ baseline, with idle-mode failures eliminated if baseline
had any; (b) phase-1 repros still pass; (c) TSAN stress run clean.

**Files (branch `phase2-experiment-B` only):**
- Modify: `src/core/Hardware/EmCPU68K.cpp` (includes + `ExecuteStoppedLoop`)

- [ ] **Step 1: Create the branch and write the hook**

```bash
git checkout -b phase2-experiment-B
```

`src/core/Hardware/EmCPU68K.cpp` — add two includes after the existing
`#include "EmSession.h"` line:

```cpp
#include "Patches/EmPatchState.h"	// UIInitialized
#include "ROMStubs.h"			// EvtWakeup
```

In `ExecuteStoppedLoop`, immediately after the `do { ... } while
(regs.spcflags & SPCFLAG_STOP);` loop and before the final `return false;`
(currently `EmCPU68K.cpp:995-997`), add:

```cpp
	// Phase 2 wake (approach B): a pen/key event posted while the guest
	// slept in STOP never signals the ROM's event group, so once the
	// pending interrupt is serviced the kernel idle loop would re-enter
	// STOP without SysEvGroupWait ever returning.  Here we are on the CPU
	// thread, ProcessInterrupt has just cleared regs.stopped, and we sit
	// at interrupt entry — the documented-legal point for EvtWakeup
	// (Palm OS permits it from interrupt handlers; see ROMStubs.cpp's
	// comment block on EvtWakeup).  Signal the event group so the UI task
	// wakes and PuppetString delivers on the next EvtGetEvent trap.
	// Worst-case added latency: one timer tick.  The tick rate naturally
	// debounces repeated wakes if the guest re-sleeps with events pending.
	if ((session->HasPenEvent () || session->HasKeyEvent ())
		&& !EmHAL::GetAsleep ()
		&& EmPatchState::UIInitialized ())
	{
		::EvtWakeup ();
	}

	return false;
```

(`session` is the existing local; the only other exit from this function is
the `CheckForBreak` `return true` path, which must NOT get the hook — it is a
host-side suspend, not a guest wake.)

- [ ] **Step 2: Build and run the delivery matrix on the branch**

```bash
cmake --build build -j$(nproc)
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed max
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed max
```

Expected: ≥ baseline everywhere; idle-mode failures gone if baseline had any.

- [ ] **Step 3: Regression + race check**

```bash
for t in tests/phase1/repro_*.py; do python3 "$t" || echo "FAIL $t"; done
python3 tests/phase2/test_speed_cmd.py
cmake --build build-tsan -j$(nproc)
python3 test_recontrol_stress.py --help   # confirm invocation flags first
```

Then run the stress suite against the TSAN build exactly as GATE 1 did
(documented form: `python3 test_recontrol_stress.py --psf m515.psf --port 6427`;
point it at the `build-tsan` binary per its `--help`).
Expected: 13/13 PASS, zero TSAN reports mentioning `EvtWakeup`,
`ExecuteStoppedLoop`, or the delivery queues.

- [ ] **Step 4: Record the experiment outcome**

```bash
git add src/core/Hardware/EmCPU68K.cpp
git commit -m "experiment(phase2): approach B STOP-exit EvtWakeup hook"
git checkout master && cmake --build build -j$(nproc)
```

Write the results (delivery matrix, repro pass/fail, TSAN verdict) into the
`docs/STATUS.md` Phase 2 bullet on master and commit:

```bash
git add docs/STATUS.md
git commit -m "docs(phase2): approach-B experiment results"
```

---

### Task 5: CHECKPOINT — decide the mechanism (recovery-plan 2.2)

**Decision rule (handoff §10 Q-MECH):**

| Evidence | Winner |
|---|---|
| B experiment passed (Task 4 criteria) | **B + C** — preserves guest-visible event timing and idle sleep; ≤1-tick latency cost. Pick this even if A's idle cost measured small. |
| B failed/flaky AND A's 1x idle cost acceptable vs. the protected baseline | **A + C** (accepting the documented guest-visible nilEvent-flood caveat) |
| Baseline failure rate was 0 in ALL FOUR runs (Q-B4: the asleep gap never manifests) | **natural-delivery + C** — no artificial wake mechanism at all; C still ships (honesty), 2.4 still ships (drops). Record WHY (e.g. apps poll with finite timeouts). |

- [ ] **Step 1:** Write the decision + the data that drove it into:
  - `docs/architecture.md` — replace the "Phase 2 decisions" blockquote's
    mechanism sentence with the outcome;
  - the handoff doc §10 Q-MECH — mark RESOLVED with the data;
  - `docs/recovery-plan-2026-06.md` — check the 2.2 box, note the choice.
- [ ] **Step 2:** Commit: `git commit -am "docs(phase2): 2.2 mechanism decision — <choice> (data-backed)"`
- [ ] **Step 3:** **Session break here (R6).** Update the recovery-plan
  CURRENT POSITION banner to "Phase 2 implementation: Task 6".

---

### Task 6: Honesty plumbing in EmSession (C layer + Q-DROP statuses)

**Files:**
- Modify: `src/core/EmSession.h` (~line 171 events, ~614 members, post decls)
- Modify: `src/core/EmSession.cpp` (`PostKeyEvent:1954`, `PostPenEvent:1995`, ctor `:112` area)
- Modify: `src/core/Patches/EmPatchMgr.cpp` (PuppetString `:1056`, `:1071`)
- Test: `tests/phase2/test_honest_ack.py` (written in Task 7 Step 1 — Tasks 6+7
  form one red-green cycle; commit at the end of Task 7)

- [ ] **Step 1: Declare the result enum and API** in `src/core/EmSession.h`.

Above `class EmSession` (near the event structs at ~line 171):

```cpp
// Result of posting host input toward the guest.  Anything but
// kInputPosted means the event was NOT queued — callers that promised
// honesty (ReControl) must surface it (recovery-plan task 2.4).
enum EmPostInputResult
{
	kInputPosted,
	kInputDroppedGremlins,
	kInputDroppedReplay,
	kInputDroppedMinimize,
	kInputDroppedDuplicate		// pen-down identical to the previous one
};
```

Change the two declarations inside `class EmSession`:

```cpp
		EmPostInputResult		PostKeyEvent		(const EmKeyEvent&);
		EmPostInputResult		PostPenEvent		(const EmPenEvent&);
```

Add next to them (public):

```cpp
		// Phase 2 delivery accounting (handoff §10 Q-SYNC).  "Delivered" =
		// PuppetString handed the event to the Palm OS event queue
		// (StubAppEnqueueKey/Pt returned) — the same guarantee real
		// hardware gives.  Per-queue counters: keys and pens are separate
		// FIFOs in PuppetString, and a shared counter could be satisfied
		// by the other queue's traffic.
		void					NotifyKeyEventDelivered	(void);	// CPU thread
		void					NotifyPenEventDelivered	(void);	// CPU thread
		uint64					KeyEventsPosted		(void);
		uint64					PenEventsPosted		(void);
		Bool					WaitForKeyDelivery	(uint64 targetSeq, int timeoutMs);
		Bool					WaitForPenDelivery	(uint64 targetSeq, int timeoutMs);
```

Add private members (near `fPenEventLock`, ~line 700):

```cpp
		// Delivery counters (Phase 2).  Dedicated lock — do NOT fold into
		// fSharedLock (lock-ordering risk with the suspend machinery).
		omni_mutex				fDeliveryLock;
		omni_condition			fDeliveryCondition;
		uint64					fKeyPostedSeq{0};
		uint64					fKeyDeliveredSeq{0};
		uint64					fPenPostedSeq{0};
		uint64					fPenDeliveredSeq{0};
```

- [ ] **Step 2: Initialize the condition** in the `EmSession` constructor init
list (`EmSession.cpp:112` area), mirroring `fSharedCondition (&fSharedLock)`:

```cpp
	fDeliveryCondition (&fDeliveryLock),
```

(immediately after the `fSleepCondition` initializer; match the existing
order-of-declaration.)

- [ ] **Step 3: Rewrite the post functions** in `src/core/EmSession.cpp`.
`PrvCanBotherCPU` STAYS — `SetButtonDown`/`SetButtonTap` still use it — but
the pen/key posts get explicit checks so the reason survives:

```cpp
EmPostInputResult EmSession::PostKeyEvent (const EmKeyEvent& event)
{
	if (Hordes::IsOn ())
		return kInputDroppedGremlins;
	if (EmEventPlayback::ReplayingEvents ())
		return kInputDroppedReplay;
	if (EmMinimize::IsOn ())
		return kInputDroppedMinimize;

	fKeyQueue.Put (event);

	{
		omni_mutex_lock lock (fDeliveryLock);
		++fKeyPostedSeq;
	}

	// Events are picked up by the SysEvGroupWait headpatch in EmPatchMgr;
	// if the guest is asleep in STOP, the wake mechanism (see
	// ExecuteStoppedLoop) gets it moving.

	return kInputPosted;
}
```

```cpp
EmPostInputResult EmSession::PostPenEvent (const EmPenEvent& event)
{
	if (Hordes::IsOn ())
		return kInputDroppedGremlins;
	if (EmEventPlayback::ReplayingEvents ())
		return kInputDroppedReplay;
	if (EmMinimize::IsOn ())
		return kInputDroppedMinimize;

	omni_mutex_lock	lock (fPenEventLock);

	// If this pen-down event is the same as the last pen-down event,
	// it would be invisible to the guest — report it instead of lying.

	if (event.fPenIsDown && event == fLastPenEvent)
		return kInputDroppedDuplicate;

	fPenQueue.Put (event);
	fLastPenEvent = event;

	{
		omni_mutex_lock dlock (fDeliveryLock);
		++fPenPostedSeq;
	}

	return kInputPosted;
}
```

(Delete the now-stale `PrvWakeUpCPU` comment paragraphs inside both — Task 8
removes the function itself.)

- [ ] **Step 4: Add the notify/wait implementations** (`EmSession.cpp`, after
`GetPenEvent`):

```cpp
// ---------------------------------------------------------------------------
//		� EmSession delivery accounting (Phase 2)
// ---------------------------------------------------------------------------
// Notify* run on the CPU thread (PuppetString, after the ROM enqueue stub
// returned).  Wait* run on the CPUWorkerThread (ReControl handlers).  The
// deadline is computed ONCE before the wait loop (Do-Not-Do #3).

void EmSession::NotifyKeyEventDelivered (void)
{
	omni_mutex_lock lock (fDeliveryLock);
	++fKeyDeliveredSeq;
	fDeliveryCondition.broadcast ();
}

void EmSession::NotifyPenEventDelivered (void)
{
	omni_mutex_lock lock (fDeliveryLock);
	++fPenDeliveredSeq;
	fDeliveryCondition.broadcast ();
}

uint64 EmSession::KeyEventsPosted (void)
{
	omni_mutex_lock lock (fDeliveryLock);
	return fKeyPostedSeq;
}

uint64 EmSession::PenEventsPosted (void)
{
	omni_mutex_lock lock (fDeliveryLock);
	return fPenPostedSeq;
}

static Bool PrvWaitForSeq (omni_mutex& mutex, omni_condition& cond,
						   uint64& seq, uint64 targetSeq, int timeoutMs)
{
	unsigned long deadline_sec = 0, deadline_nsec = 0;
	omni_thread::get_time (&deadline_sec, &deadline_nsec,
						   timeoutMs / 1000,
						   (timeoutMs % 1000) * 1000000UL);

	omni_mutex_lock lock (mutex);
	while (seq < targetSeq)
	{
		if (cond.timedwait (deadline_sec, deadline_nsec) == 0)	// 0 = timeout
			return seq >= targetSeq;
	}
	return true;
}

Bool EmSession::WaitForKeyDelivery (uint64 targetSeq, int timeoutMs)
{
	return ::PrvWaitForSeq (fDeliveryLock, fDeliveryCondition,
							fKeyDeliveredSeq, targetSeq, timeoutMs);
}

Bool EmSession::WaitForPenDelivery (uint64 targetSeq, int timeoutMs)
{
	return ::PrvWaitForSeq (fDeliveryLock, fDeliveryCondition,
							fPenDeliveredSeq, targetSeq, timeoutMs);
}
```

- [ ] **Step 5: Notify from PuppetString** (`src/core/Patches/EmPatchMgr.cpp`):

After `::StubAppEnqueueKey (event.fKey, 0, modifiers);` (currently `:1056`):

```cpp
			gSession->NotifyKeyEventDelivered ();
```

After `StubAppEnqueuePt (&palmPen);` (currently `:1071`):

```cpp
			gSession->NotifyPenEventDelivered ();
```

- [ ] **Step 6: Build**

Run: `cmake --build build -j$(nproc)`
Expected: clean build. The ignored-return callers (`EmWindow.cpp:272`,
`EmDocument.cpp:461`, `ReControl.cpp:291`) compile unchanged.

---

### Task 7: The honest contract in ReControl (Q-ACK option a + 2.4)

**Files:**
- Test: `tests/phase2/test_honest_ack.py` (NEW — write FIRST)
- Modify: `src/core/ReControlCmds_Input.cpp` (tap/pen/key/type/tap-id/button)
- Modify: `src/core/ReControl.cpp` (categories; validate in WorkerRaw path)
- Modify: `src/core/ReControl.h` (category comments)
- Modify: `src/core/EmSession.h/.cpp` (`SetButtonDown`/`SetButtonTap` → `Bool`)
- Modify: `tests/phase1/repro_1_8_argval.py` (expected strings)
- Modify: `docs/recontrol-protocol.md` (Input table — Task 8 commit includes it)

- [ ] **Step 1: Write the failing test**

`tests/phase2/test_honest_ack.py`:

```python
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 tests/phase2/test_honest_ack.py`
Expected: `AssertionError` on the first check (`tap 80 80` returns `OK`, not
`OK delivered`).

- [ ] **Step 3: Rewrite the input handlers** in
`src/core/ReControlCmds_Input.cpp`.

Add near the top (after the validators, ~line 70):

```cpp
// Map a refused post to the honest protocol error (recovery-plan 2.4).
static std::string PrvInputDropError (EmPostInputResult r)
{
	switch (r)
	{
		case kInputDroppedGremlins:
			return "ERR busy: gremlin running\n";
		case kInputDroppedReplay:
			return "ERR busy: event playback active\n";
		case kInputDroppedMinimize:
			return "ERR busy: minimization active\n";
		case kInputDroppedDuplicate:
			return "ERR duplicate: pen already down at that point\n";
		default:
			return "ERR transient: event not posted\n";
	}
}

static const int kDeliveryTimeoutMs = 2000;
static const char* kPendingError =
	"ERR pending: queued, not delivered within 2000ms\n";
```

Replace `RcCmd_Tap`:

```cpp
std::string RcCmd_Tap (const QStringList& args)
{
	if (args.size () != 3)
		return "ERR usage: tap <x> <y>\n";

	int x = args[1].toInt ();
	int y = args[2].toInt ();

	EmPenEvent penDown (EmPoint (x, y), true);
	EmPostInputResult r = gSession->PostPenEvent (penDown);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	EmPenEvent penUp (EmPoint (-1, -1), false);
	r = gSession->PostPenEvent (penUp);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
}
```

Replace `RcCmd_Pen`'s tail (after building `penEvent`):

```cpp
	EmPenEvent penEvent (EmPoint (x, y), isDown);
	EmPostInputResult r = gSession->PostPenEvent (penEvent);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
```

Replace `RcCmd_Key`'s tail:

```cpp
	int charcode = args[1].toInt ();
	EmKeyEvent keyEvent (charcode);
	EmPostInputResult r = gSession->PostKeyEvent (keyEvent);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->KeyEventsPosted ();
	if (gSession->WaitForKeyDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
```

Replace `RcCmd_Type`'s posting loop and tail:

```cpp
	for (int i = 0; i < latin1.size (); i++)
	{
		unsigned char ch = (unsigned char) latin1[i];
		EmKeyEvent keyEvent (ch);
		EmPostInputResult r = gSession->PostKeyEvent (keyEvent);
		if (r != kInputPosted)
			return ::PrvInputDropError (r);
	}

	uint64 target = gSession->KeyEventsPosted ();
	if (gSession->WaitForKeyDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered\n";
	return kPendingError;
```

Restructure `RcCmd_TapId`: it currently runs entirely under the dispatch
loop's stopper (WorkerCycle). Waiting for delivery while OUR OWN stopper
holds the CPU would deadlock, so the stopper must scope only the form lookup.
Wrap the existing lookup code (from `CEnableFullAccess munge;` through the
object loop) in a block that computes `cx`/`cy`, then post + wait outside it:

```cpp
std::string RcCmd_TapId (const QStringList& args)
{
	if (args.size () != 2)
		return "ERR usage: tap-id <object_id>\n";

	int targetId = args[1].toInt ();
	int cx = -1, cy = -1;

	{
		// Stop the CPU only while reading guest memory.  The stopper MUST
		// be released before WaitForPenDelivery, or delivery could never
		// happen and we'd always time out.
		EmSessionStopper stopper (gSession, kStopOnCycle, 5000);
		if (!stopper.Stopped ())
			return "ERR timeout: CPU did not reach a cycle boundary within 5000ms. "
			       "Recovery: dismiss any dialog (dialog respond) or palm_reset.\n";

		CEnableFullAccess munge;

		/* ... existing lookup code unchanged, except the match case ends:
		   cx = winX + bx + bw / 2;
		   cy = winY + by + bh / 2;
		   break-out instead of posting here ... */
	}

	if (cx < 0)
		return "ERR usage: object " + std::to_string (targetId) + " not found\n";

	EmPenEvent penDown (EmPoint (cx, cy), true);
	EmPostInputResult r = gSession->PostPenEvent (penDown);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);
	EmPenEvent penUp (EmPoint (-1, -1), false);
	r = gSession->PostPenEvent (penUp);
	if (r != kInputPosted)
		return ::PrvInputDropError (r);

	uint64 target = gSession->PenEventsPosted ();
	if (gSession->WaitForPenDelivery (target, kDeliveryTimeoutMs))
		return "OK delivered " + std::to_string (cx) + " " + std::to_string (cy) + "\n";
	return kPendingError;
}
```

(Convert the loop's "found" path to set `cx`/`cy` and `break` from the loop;
`continue`-on-mismatch stays.)

`RcCmd_Button`: make the silent drop honest, keep the queued contract.
Change `EmSession::SetButtonDown` and `SetButtonTap` to return `Bool`
(`false` when `PrvCanBotherCPU()` refuses; `SetButtonUp` stays `void`), and
in `RcCmd_Button` return `"ERR busy: gremlin or playback active\n"` when the
relevant call returns false, else the existing `"OK\n"`. The internal caller
`ExecuteStoppedLoop` (`EmCPU68K.cpp:865`) ignores the return — it toggles
Hordes off first, so it cannot be refused.

- [ ] **Step 4: Recategorize in the dispatch table** (`src/core/ReControl.cpp`).

Change the five rows:

```cpp
	{"tap",            kCmdWorkerRaw,     0,    RcCmd_Tap,        nullptr, RcValidate_Tap},
	{"tap-id",         kCmdWorkerRaw,     0,    RcCmd_TapId,      nullptr, nullptr},
	{"pen",            kCmdWorkerRaw,     0,    RcCmd_Pen,        nullptr, RcValidate_Pen},
	{"key",            kCmdWorkerRaw,     0,    RcCmd_Key,        nullptr, RcValidate_Key},
	{"type",           kCmdWorkerRaw,     0,    RcCmd_Type,       nullptr, RcValidate_Type},
```

(`button` stays `kCmdWorkerDirect`.) In the `kCmdWorkerRaw` dispatch case
(currently `:501-509`), add main-thread validation before queueing, exactly
mirroring the WorkerDirect case (this preserves the task-1.8 guarantee):

```cpp
		case kCmdWorkerRaw:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			if (entry->validate)
			{
				std::string verr = entry->validate (parts);
				if (!verr.empty ()) { Send (verr); return; }
			}
			auto handler = entry->handler;
			QueueWorkResult ([handler, parts]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				return handler (parts);
			});
			break;
		}
```

Update the comment in `src/core/ReControl.h:27-30`: `kCmdWorkerDirect` —
"worker thread, no stopper, always sends OK (hardware-path input only)";
`kCmdWorkerRaw` — "worker thread, no stopper (handler creates own); runs
main-thread validate if present".

- [ ] **Step 5: Update the phase-1 exact-match repro**

`tests/phase1/repro_1_8_argval.py` — replace the `good` dict with an ordered
list (power tap LAST: it puts the device to sleep, and anything queued after
it would honestly be `ERR pending`):

```python
        good = [
            ("tap 80 80", "OK delivered"),
            ("tap 130 8", "OK delivered"),
            ("key 65", "OK delivered"),
            ("pen down 40 40", "OK delivered"),
            ("pen up 40 40", "OK delivered"),
            ("type hello", "OK delivered"),
            ("button power tap", "OK"),
        ]
        regressions = []
        for cmd, want in good:
```

(and update the file's comment: well-formed input now returns the honest
delivered/err contract — landmine #3 is closed by Phase 2.)

- [ ] **Step 6: Build, run the new test and the regression set**

```bash
cmake --build build -j$(nproc)
python3 tests/phase2/test_honest_ack.py
for t in tests/phase1/repro_*.py; do python3 "$t" || echo "FAIL $t"; done
python3 tests/phase2/test_delivery.py --mode rapid --count 20
```

Expected: `PASS honest ACK contract`; all repros PASS; delivery test
unaffected (it asserts effects, and now logs `OK delivered` responses).

- [ ] **Step 7: Commit (Tasks 6+7 together — one logical change)**

```bash
git add src/core/EmSession.h src/core/EmSession.cpp \
        src/core/Patches/EmPatchMgr.cpp src/core/ReControl.cpp \
        src/core/ReControl.h src/core/ReControlCmds_Input.cpp \
        tests/phase2/test_honest_ack.py tests/phase1/repro_1_8_argval.py
git commit -m "feat(phase2): honest input contract — delivered-ACK + truthful drop errors (2.4)"
```

---

### Task 8: Land the winning mechanism + delete the losers (2.2 + 2.3, ONE commit)

Written for the expected winner **B** (adjust per the Task 5 record: for A,
apply the Task 3 edits instead of the hook and say so in the docs; for
natural+C, only the deletions + docs land).

**Files:**
- Modify: `src/core/Hardware/EmCPU68K.cpp` (the Task 4 hook, re-applied)
- Modify: `src/core/EmSession.cpp` (delete `PrvWakeUpCPU` decl `:60-61` + body `:2092-2135`; delete the stale "bridge thread's PaintScreen" wording at `:1325` — reword to "the UI thread's PaintScreen")
- Modify: `src/core/EmWindow.cpp:489` ("bridge thread" → "UI thread")
- Modify: `src/core/CPUWorkerThread.h:15-21` (drop the `PrvWakeUpCPU` rationale sentence; the thread exists to keep `EmSessionStopper` etc. off the Qt main thread)
- Modify: `docs/architecture.md`, `docs/STATUS.md`, `docs/recontrol-protocol.md`, `docs/recovery-plan-2026-06.md`
- Delete: `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`

- [ ] **Step 1: Re-apply the winner** — cherry-pick the experiment commit
(`git cherry-pick <sha-from-task-4>`) or re-apply the same edit by hand.

- [ ] **Step 2: Delete the losers in the same working tree:**
  - `PrvWakeUpCPU` — declaration and definition in `EmSession.cpp` (verify
    first: `grep -rn "PrvWakeUpCPU" src/` must show only comments slated for
    rewording);
  - the three stale comments listed above;
  - the 2026-03-13 patch file (`git rm`);
  - the A measurement branch: `git branch -D phase2-measure-A phase2-experiment-B`.

- [ ] **Step 3: Doc updates (same commit, R5):**
  - `docs/recontrol-protocol.md` Input table: `tap`/`tap-id`/`pen`/`key`/`type`
    → `OK delivered\n` (tap-id: `OK delivered <x> <y>\n`) with the
    `ERR pending` / `ERR busy: …` / `ERR duplicate: …` rows described;
    `button` → note "ERR busy when gremlin/playback active".
  - `docs/architecture.md`: rewrite Do-Not-Do #9 to describe the landed
    mechanism ("posted input may not sit undelivered while the guest sleeps;
    the STOP-exit EvtWakeup hook in ExecuteStoppedLoop is the ONE wake
    mechanism — do not add a second"); reword #8 past-tense ("the OLD
    PrvWakeUpCPU, removed in Phase 2, deadlocked because…"); update the
    PuppetString-logic section and the "Idle sleep status" paragraph to
    describe actual landed behavior; remove the now-stale "(Status note
    2026-06-10…)" qualifier.
  - `docs/STATUS.md`: landmine #3 → ~~struck~~ **FIXED (Phase 2, date,
    commit)** with mechanism + repro pointers (`tests/phase2/`); remove the
    patch-file reference in "Working tree state"; Phase 2 bullet updated.
  - `docs/recovery-plan-2026-06.md`: check boxes 2.1–2.4.

- [ ] **Step 4: Full verification before committing**

```bash
cmake --build build -j$(nproc)
python3 tests/phase2/test_honest_ack.py
python3 tests/phase2/test_speed_cmd.py
for t in tests/phase1/repro_*.py; do python3 "$t" || echo "FAIL $t"; done
grep -rn "PrvWakeUpCPU" src/        # expected: no output
grep -rn "EvtWakeup" src/ | grep -v ROMStubs   # expected: exactly the one hook (B) or nothing (A/natural)
```

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "feat(phase2): land <winner> wake mechanism; delete PrvWakeUpCPU + losers (2.2+2.3)"
```

---

### Task 9: GATE 2

- [ ] **Step 1: The 200-tap matrix**

```bash
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed 100
python3 tests/phase2/test_delivery.py --mode rapid --count 100 --speed max
python3 tests/phase2/test_delivery.py --mode idle  --count 100 --speed max
```

Pass: ≥ 99% per speed (≤ 2 failures across the 200 taps at each speed).

- [ ] **Step 2: Idle CPU with the landed mechanism** — rerun Task 3 Step 2
(3× at 1x, 3× at Max); record medians in `docs/STATUS.md` next to baseline
(B should be within noise of baseline; that's the headline-feature check).

- [ ] **Step 3: Race check** — TSAN build + the GATE 1 stress suite
(13 scenarios), expected 13/13 PASS, no reports.

- [ ] **Step 4: The grep gate** — `grep -rn "EvtWakeup\|PrvWakeUpCPU" src/`
output recorded in STATUS: exactly one wake mechanism (plus the ROMStubs stub
definition).

- [ ] **Step 5: Record, tag, push**

Update `docs/STATUS.md` (GATE 2 PASSED line with all numbers) and the
recovery-plan CURRENT POSITION banner (Phase 2 complete → next: Phase 3).

```bash
git add docs/STATUS.md docs/recovery-plan-2026-06.md
git commit -m "docs(phase2): GATE 2 passed — delivery matrix, idle CPU, grep gate"
git tag phase-2-complete
git push && git push --tags
```

---

## Self-review notes

- **Spec coverage:** 2.1 = Task 2; 2.2 = Tasks 3+4+5+8; 2.3 = Task 8; 2.4 =
  Tasks 6+7; GATE 2 = Task 9; Q-SPEED prerequisite = Task 1. All handoff §10
  decisions have a landing task.
- **Known judgment points for the executor** (not placeholders — verified
  unknowables until run time): the exact Datebook button labels (Task 2 Step 1
  verifies before the test is finalized); the stress-suite CLI flags (checked
  via `--help` before use); the checkpoint branch in Task 8 follows the Task 5
  record.
- **Type consistency:** `EmPostInputResult` + `kInput*` names, `NotifyKey/
  PenEventDelivered`, `WaitForKey/PenDelivery`, `Key/PenEventsPosted` are used
  identically in Tasks 6, 7; `RcCmd_Speed` matches its forward declaration.
