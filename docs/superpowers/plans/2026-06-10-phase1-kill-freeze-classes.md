# Phase 1 — Kill the Freeze Classes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to
> implement this plan task-by-task (inline, strongest model — R6 requires the
> strongest model for threading work, so do NOT delegate the C++ threading fixes
> to weaker subagents). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An AI agent can hammer the emulator for 30 minutes — including crash
dialogs, concurrent GUI use, and client disconnects — without a freeze, a lie,
or a process restart (GATE 1).

**Architecture:** Stabilization-only. Eight verified control-plane defects in
`EmSession`, `CPUWorkerThread`, `ReControl`, `EmWindow`, and the MCP proxy. Each
fix lands with a committed reproduction first (R1); each superseded mechanism is
deleted, not stranded (R2); every input/launch assertion checks emulator effects,
not `OK` (R3); the tree is clean per task (R4); protocol/behavior changes update
docs in the same commit (R5).

**Tech stack:** Qt6/C++17, CMake, omni_thread, ASAN/TSAN, Python (`test_recontrol.py`
`ReControlClient` + `test_recontrol_stress.py`).

> **REVISION 2026-06-10 (plan designer):** Task **1.0d** (dialog-action lifetime
> fix, landmine #9) is inserted before 1.1 — full plan:
> `docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md`. Live
> re-verification **disproved** the "dialog never shows" hang from the
> 2026-06-10 finding: dialogs show one idle tick (~100 ms) after
> `blocked_on_ui`, and `dialog` / `dialog respond continue` work. Therefore
> 1.1/1.2 are NOT hang-blocked; they follow 1.0d only so `reset` is a safe
> recovery while their repros raise dialogs. For Task 1.1's `_force_blocked`:
> candidate (c) is verified — `spy set 0x134` reaches `blocked_on_ui` in
> ~50 ms; dismiss with `dialog respond continue`; `spy clear` stops re-fires
> (the spy re-fires within ~10 ms of resuming, so clear it in the
> respond→clear loop or finish with a post-1.0d `reset`).
> Status: 1.0 (harness) done; 1.8, 1.3 done & verified; 1.0d next, then 1.1.

---

## Binding process rules (from docs/recovery-plan-2026-06.md)

- **R1** Reproduce first — every fix commit carries a failing repro committed with/before it.
- **R2** Replace, don't stack — deleting/quarantining the superseded mechanism in the same commit.
- **R3** Effects, not responses — assert screen-hash/`ui`/`apps`/`state` change, never just `OK`.
- **R4** Clean tree per task — no surviving debug instrumentation; commit-or-revert.
- **R5** Docs in the same commit — `docs/recontrol-protocol.md` + `SKILL.md` + `STATUS.md` (verified facts only).
- **R6** One phase-task per sitting where feasible; strongest model; run the gate before stopping.

## Environment & harness facts (verified this session)

- Build dir: `build/` (legacy, no presets). Build: `cmake --build build -j16`. Binaries:
  `build/pose64`, `build/pose64-mcp-proxy`. Baseline build is green (exit 0).
- Launch headless: `build/pose64 -psf <file> --port <N>`. If no X display is present,
  prefix `QT_QPA_PLATFORM=offscreen` (confirm at first repro run).
- ROMs/sessions in repo root: `m515.psf`, `m500.psf`, `freshm515.psf`, plus raw `.rom`
  files (`Palm-m515-4.1-en.rom`, `Palm-Vx-4.0-en.rom`, `Palm-m500-4.1-en.rom`, …).
- Ports: 6416 dev, 6425 secondary, 6427 testing. Use a per-repro port (6431+) to avoid
  colliding with any running instance. One session at a time (extra clients get `ERR busy`).
- `ReControlClient(host="localhost", port=N, timeout=S)`: `.connect()->bool`,
  `.send_command(str)->str` (single line), `.connected`, `.socket` (raw, for multi-line /
  disconnect-mid-command), `.disconnect()`.
- Reproductions live in `tests/phase1/` (new dir). Each is standalone, self-launches a
  `pose64` on its own port, asserts effects, exits 0=pass / non-zero=fail. GATE 1 scenarios
  are added to `test_recontrol_stress.py`.

## Sanitizer builds (for GATE 1 and the race tasks 1.5/1.7)

- ASAN build dir `build-asan/`, TSAN build dir `build-tsan/`, configured with
  `-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -g -O1"` (and `=thread`
  for TSAN). `ubsan.supp` already exists in root. These are created in Task 1.0.

---

## File structure

| File | Responsibility | Tasks |
|------|----------------|-------|
| `src/core/EmSession.cpp` | `SuspendThread`/`ResumeThread`/`ForceReset` counter discipline; `PostPenEvent` race | 1.1, 1.2, 1.5, 1.7 |
| `src/core/EmSession.h` | `fPenEventLock` member; stopper default timeout | 1.2, 1.7 |
| `src/core/CPUWorkerThread.cpp/.h` | bounded `shutdown()` + escalation | 1.4 |
| `src/core/ReControl.cpp` | dispatch timeouts; main-thread arg validation | 1.2, 1.8 |
| `src/core/ReControl.h` | `RcCommandEntry::validate` field | 1.8 |
| `src/core/ReControlCmds_Input.cpp` | `RcValidate_*` validators | 1.8 |
| `src/core/EmWindow.cpp` | `PaintScreen` screen-read safety | 1.6 |
| `src/pose64-mcp-proxy.cpp` | `SO_RCVTIMEO`; no double-execute; structured timeouts | 1.3 |
| `tests/phase1/*.py` | per-task reproductions | all |
| `test_recontrol_stress.py` | GATE 1 scenarios | GATE 1 |
| `docs/recontrol-protocol.md`, `docs/STATUS.md`, `docs/architecture.md`, `claude/skills/palm-dev/SKILL.md` | doc truth (R5) | per task |

---

## Task 1.0: Harness scaffolding (no behavior change)

> **STATUS: harness DONE & committed 2026-06-10 (Steps 1, 2, 4). Step 3 is
> STILL OPEN — `build-asan/`/`build-tsan/` were never configured; do it at
> first need (1.0d Step 10, tasks 1.5-1.7). `build-*/` is already gitignored.**

**Files:** Create `tests/phase1/_harness.py`, `tests/phase1/__init__.py`; create
`build-asan/`, `build-tsan/` via cmake.

- [x] **Step 1: Reusable launcher.** Create `tests/phase1/_harness.py` with a context
  manager that self-launches `pose64` on a given port with `QT_QPA_PLATFORM=offscreen`
  (falling back to inherited DISPLAY), waits for `state`==OK, yields a connected
  `ReControlClient`, and tears the process down (terminate, then kill). Reuse
  `wait_for_server` semantics from `test_recontrol_stress.py`.

```python
import os, subprocess, sys, time, contextlib
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from test_recontrol import ReControlClient

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def wait_ready(port, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        c = ReControlClient(port=port, timeout=2)
        if c.connect():
            r = c.send_command("state"); c.disconnect()
            if r and r.startswith("OK"):
                return True
        time.sleep(0.4)
    return False

@contextlib.contextmanager
def emulator(port, psf="m515.psf", build="build", env_extra=None):
    exe = os.path.join(REPO, build, "pose64")
    env = dict(os.environ); env.setdefault("QT_QPA_PLATFORM", "offscreen")
    if env_extra: env.update(env_extra)
    proc = subprocess.Popen([exe, "-psf", os.path.join(REPO, psf), "--port", str(port)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, env=env)
    try:
        if not wait_ready(port):
            raise RuntimeError(f"server not ready on {port}")
        yield proc
    finally:
        if proc.poll() is None:
            proc.terminate()
            try: proc.wait(timeout=5)
            except subprocess.TimeoutExpired: proc.kill(); proc.wait()
```

- [x] **Step 2: Verify offscreen launch works.**
  Run: `cd tests/phase1 && python3 -c "import _harness; _harness.__dict__"` then a one-liner
  that opens `emulator(6431)` and prints `state`. Expected: `OK running` (or `OK suspended:*`).
  If `QT_QPA_PLATFORM=offscreen` is rejected, fall back to `xvfb-run` and record which works.

- [ ] **Step 3: Sanitizer build dirs (STILL OPEN as of 2026-06-10).**
  Run: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"`
  and the `=thread` equivalent into `build-tsan`. Do NOT build yet (build per-task to save time).
  Append `build-asan/` and `build-tsan/` to `.gitignore`.

- [x] **Step 4: Commit.**
```bash
git add tests/phase1/_harness.py tests/phase1/__init__.py .gitignore
git commit -m "test: Phase 1 reproduction harness (offscreen self-launch)"
```

---

## Task 1.0d: Dialog-action lifetime fix (landmine #9) — DANGEROUS, goes before 1.1

**Added 2026-06-10.** `reset` (or stop) while a deferred-error dialog is queued
or showing UAFs the CPU thread's `BlockOnDialog` stack frame — two verified
lethal interleavings (queued → SIGSEGV read; showing → dangling `fDlgResult`
write). Cross-thread lifetime fix in `EmSession`/`EmDocument`; R6 applies
(strongest model, single sitting).

**Full step-by-step plan (do not improvise from this stub):**
`docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md`

**Gate:** `tests/phase1/repro_dialog_subsystem.py` (extended: both
interleavings + show-latency guard) passes 3× on `build/` and once ASAN-clean
on `build-asan/`. Unblocks 1.1/1.2.

---

## Task 1.1: Suspend-counter leak (landmine #2 — the "permanently locked emulation" class)

**Mechanism (verified against current code):** `EmSession::SuspendThread`
(`src/core/EmSession.cpp:759`) increments `fSuspendByUIThread` unconditionally for
`kStopNow` (786) and `kStopOnCycle` (790) in the *first* switch — before the
`if (fState == kRunning)` block. When a dialog is up the CPU is `kBlockedOnUI`, so that
block is skipped; the *result* switch then sets `result = (fState == kSuspended)` for
`kStopOnCycle` (974) → **false**, and the function returns at 998 **without decrementing**.
Per the documented contract (755-757) the caller (`EmSessionStopper`) then does NOT call
`ResumeThread`, so the increment leaks. Next time the CPU leaves `kBlockedOnUI` it sees a
non-zero counter and parks forever. `ForceReset` (2266) deliberately won't clear it.

**Files:**
- Modify: `src/core/EmSession.cpp` (result switch ~961-994; `ForceReset` ~2254-2283)
- Test: `tests/phase1/repro_1_1_suspend_leak.py`

- [ ] **Step 1: Write the failing reproduction.** Create
  `tests/phase1/repro_1_1_suspend_leak.py`. Raise a dialog, then issue a `kCmdWorkerCycle`
  command (`ui`) while blocked, dismiss, assert `state`==running.

```python
#!/usr/bin/env python3
"""Repro 1.1: kStopOnCycle command while a dialog is up leaks fSuspendByUIThread,
parking the CPU forever after the dialog is dismissed.  Effect asserted: after
dismiss, `state` must return to running and a subsequent `tap` must change the screen."""
import sys
from _harness import emulator
from test_recontrol import ReControlClient

PORT = 6432

def raise_dialog(c):
    # spy on a frequently-written low-memory global; the next write raises a
    # deferred-error dialog -> blocked_on_ui.  (watch/spy raise dialogs per STATUS.md.)
    c.send_command("spy set global.JmpTblP")  # address resolved at runtime
    # nudge the CPU so a write happens
    c.send_command("tap 80 80")

def main():
    with emulator(PORT):
        c = ReControlClient(port=PORT, timeout=5); c.connect()
        # 1. get to blocked_on_ui
        # (exact dialog trigger finalized at execution; see Step 2 notes)
        st = _force_blocked(c)
        assert st.startswith("OK blocked_on_ui"), f"setup failed: {st!r}"
        # 2. issue a kCmdWorkerCycle command while blocked (this is the leak trigger)
        c.send_command("ui")
        # 3. dismiss the dialog
        c.send_command("dialog respond continue")
        # 4. EFFECT: CPU must be running again
        st = c.send_command("state")
        assert st.strip() == "OK running", f"LEAK: state={st!r} (CPU parked)"
        # 5. EFFECT: input must still land (screen changes)
        h1 = c.send_command("screen-hash")
        c.send_command("tap 80 80"); c.send_command("tap 75 75")
        h2 = c.send_command("screen-hash")
        assert h1 != h2, f"CPU not processing input after recovery: {h1!r}=={h2!r}"
        print("PASS 1.1"); c.disconnect()

if __name__ == "__main__":
    main()
```

  > Execution note: the exact reliable dialog trigger (`_force_blocked`) is finalized when
  > the live emulator is in front of us — candidates in priority order: (a) `watch set
  > <screen-addr> <n>` then `tap`; (b) `errorhandling set ErrorOn show` then `poke` an
  > illegal opcode at PC and step; (c) `spy set <global>`. Pick the one that deterministically
  > yields `blocked_on_ui` within ~2 s. The assertion (steps 4-5) is fixed and effect-based (R3).

- [ ] **Step 2: Run repro, confirm it FAILS (bug reproduces).**
  Run: `cd tests/phase1 && python3 repro_1_1_suspend_leak.py`
  Expected: `AssertionError: LEAK: state='OK suspended:...'` (CPU parked) — confirming the leak.

- [ ] **Step 3: Apply the fix.** In `src/core/EmSession.cpp`, after the result switch and
  `fBreakOnSysCall = false;` (currently line 987), add the side-effect-free failure cleanup:

```cpp
	fBreakOnSysCall = false;

	// Landmine #2 fix: kStopNow/kStopOnCycle incremented fSuspendByUIThread in
	// the first switch, but if we failed to stop the way the caller needs
	// (e.g. the CPU was kBlockedOnUI, so kStopOnCycle yields result==false) the
	// caller sees Stopped()==false and will NOT call ResumeThread.  Undo the
	// increment here so the failure path is side-effect-free; otherwise the
	// counter leaks and the CPU parks forever once it leaves kBlockedOnUI.
	if (!result && (how == kStopNow || how == kStopOnCycle))
	{
		if (fSuspendState.fCounters.fSuspendByUIThread > 0)
			--fSuspendState.fCounters.fSuspendByUIThread;
	}

	if (result)
	{
		EmAssert (fSuspendState.fCounters.fSuspendByUIThread > 0);
		// ... existing asserts unchanged ...
```

  And in `ForceReset` (`src/core/EmSession.cpp:2254`), replace the "Do NOT clear" block with a
  last-resort clear (belt-and-suspenders for the user's recovery path):

```cpp
	// Reset is the user's last-resort recovery.  Clear ALL suspend counters so
	// reset always works even if some other path leaked one.  The primary leak
	// (landmine #2) is fixed at the source in SuspendThread; this guarantees
	// recovery against any future regression.  ResumeThread's `> 0` guard makes
	// a later decrement from an outstanding stopper a safe no-op.
	fSuspendState.fCounters.fSuspendByUIThread = 0;
	fSuspendState.fCounters.fSuspendByDebugger = 0;
	fSuspendState.fCounters.fSuspendByExternal = 0;
	fSuspendState.fCounters.fSuspendBySysCall = 0;
	fSuspendState.fCounters.fSuspendBySubroutineReturn = 0;
```

- [ ] **Step 4: Rebuild.** Run: `cmake --build build -j16` — Expected: exit 0.

- [ ] **Step 5: Run repro, confirm it PASSES.**
  Run: `cd tests/phase1 && python3 repro_1_1_suspend_leak.py` — Expected: `PASS 1.1`.

- [ ] **Step 6: Commit (R1, R5).** Update `docs/STATUS.md` landmine #2 to describe the fix
  (verified) and `docs/architecture.md` CPU-Suspension section. Then:
```bash
git add tests/phase1/repro_1_1_suspend_leak.py src/core/EmSession.cpp docs/STATUS.md docs/architecture.md
git commit -m "fix: SuspendThread failure paths no longer leak fSuspendByUIThread (landmine #2)

kStopOnCycle while blocked_on_ui returned false without decrementing the
counter it incremented, parking the CPU forever after dialog dismiss.
Failure is now side-effect-free; ForceReset clears the counter as a
last-resort recovery guarantee. Repro: tests/phase1/repro_1_1_suspend_leak.py"
```

---

## Task 1.2: Universal stop timeouts

**Mechanism:** `useTimeout` (`EmSession.cpp:828`) is gated on `how == kStopOnSysCall`, so a
`kStopNow`/`kStopOnCycle` stop that never reaches its boundary (CPU wedged in a nested ROM
loop) blocks the worker thread forever; the dispatch (`ReControl.cpp:456,510`) creates those
stoppers with no timeout, and the handler never returns → every subsequent MCP call hangs.

**Files:**
- Modify: `src/core/EmSession.cpp` (`useTimeout` formula 828; timeout return path 873-882)
- Modify: `src/core/ReControl.cpp` (WorkerCycle 450-462, Adaptive 506-516 stopper timeouts)
- Modify: `src/core/EmSession.h` (stopper default timeout, if needed)
- Test: `tests/phase1/repro_1_2_stop_timeout.py`

- [ ] **Step 1: Failing reproduction.** Create `tests/phase1/repro_1_2_stop_timeout.py`:
  wedge the CPU (enter `blocked_on_ui` so a `kStopOnCycle` cannot reach `kSuspended`), then
  send a `kCmdWorkerCycle` command and assert it returns a bounded `ERR timeout:` within ~6 s
  instead of hanging.

```python
#!/usr/bin/env python3
"""Repro 1.2: a kStopOnCycle command issued while the CPU cannot reach a cycle
boundary must return ERR timeout within a bounded deadline, not hang forever."""
import sys, time
from _harness import emulator
from test_recontrol import ReControlClient
PORT = 6433
def main():
    with emulator(PORT):
        c = ReControlClient(port=PORT, timeout=12); c.connect()
        _force_blocked(c)  # same helper as 1.1 -> blocked_on_ui (CPU not at a cycle boundary)
        t0 = time.time()
        resp = c.send_command("ui")   # kCmdWorkerCycle; cannot stop at cycle while blocked
        dt = time.time() - t0
        # After 1.1, blocked_on_ui+ui no longer leaks; for 1.2 we assert the *timeout*
        # behavior on a genuinely-wedged stop.  Adjust the wedge so kStopOnCycle cannot
        # succeed; assert a bounded ERR timeout rather than an unbounded hang.
        assert resp.startswith("ERR timeout"), f"expected timeout, got {resp!r}"
        assert dt < 8.0, f"timeout not bounded: {dt:.1f}s"
        # control plane still answers afterwards
        assert c.send_command("state").startswith("OK"), "control plane wedged after timeout"
        print("PASS 1.2"); c.disconnect()
if __name__ == "__main__": main()
```

  > Execution note: 1.1 makes `blocked_on_ui + ui` recover cleanly (no leak), so the 1.2 wedge
  > must be a state where `kStopOnCycle` genuinely cannot complete (CPU spinning in a nested
  > ROM call). Finalize the wedge against the live build; the *assertion* (bounded `ERR
  > timeout`, control plane still alive) is fixed.

- [ ] **Step 2: Run, confirm FAIL** (today: hang until client timeout / no `ERR timeout`).
  Run: `cd tests/phase1 && python3 repro_1_2_stop_timeout.py`
  Expected: client-side timeout or assertion failure (no bounded `ERR timeout`).

- [ ] **Step 3: Fix — generalize the deadline in SuspendThread.** Change line 828:
```cpp
	Bool useTimeout = (timeoutMs > 0);   // was: (timeoutMs > 0 && how == kStopOnSysCall)
```
  And make the timeout return path (currently 876-881) undo the kStopNow/kStopOnCycle
  increment so it stays leak-free (consistent with 1.1):
```cpp
				if (rc == 0)  // timeout (0 = timeout, 1 = signaled)
				{
					fBreakOnSysCall = false;
					// Undo the first-switch increment (kStopNow/kStopOnCycle);
					// kStopOnSysCall never incremented here.  Keeps the timeout
					// path side-effect-free (landmines #2/#4).
					if ((how == kStopNow || how == kStopOnCycle) &&
						fSuspendState.fCounters.fSuspendByUIThread > 0)
						--fSuspendState.fCounters.fSuspendByUIThread;
					return false;
				}
```

- [ ] **Step 4: Fix — pass timeouts from dispatch.** In `ReControl.cpp` WorkerCycle (456) and
  Adaptive (510), give the stopper a deadline and report timeout:
```cpp
			EmSessionStopper stopper (gSession, kStopOnCycle, 5000);
			if (!stopper.Stopped ())
				return "ERR timeout: CPU did not reach a cycle boundary within 5000ms. "
				       "Recovery: dismiss any dialog (dialog respond) or palm_reset.\n";
```
  Verify `EmSessionStopper`'s constructor default for `timeoutMs` in `EmSession.h`; if direct
  callers rely on a 0 default, keep it and only pass explicit timeouts here.

- [ ] **Step 5: Rebuild + run repro.** `cmake --build build -j16` then
  `python3 repro_1_2_stop_timeout.py` — Expected: `PASS 1.2`. Re-run 1.1 repro — still `PASS`.

- [ ] **Step 6: Commit (R5: protocol doc gains the new `ERR timeout` for cycle stops).**
```bash
git add tests/phase1/repro_1_2_stop_timeout.py src/core/EmSession.cpp src/core/ReControl.cpp docs/recontrol-protocol.md docs/STATUS.md
git commit -m "fix: bounded timeouts for kStopNow/kStopOnCycle stops (landmine #4)

A wedged cycle/now stop no longer blocks the worker forever; WorkerCycle and
Adaptive dispatch return 'ERR timeout' with a recovery hint. Repro:
tests/phase1/repro_1_2_stop_timeout.py"
```

---

## Task 1.3: Proxy honesty — read timeout + no double-execute

> **STATUS: DONE & VERIFIED 2026-06-10, commit `08d8690`.**
> Repro: `tests/phase1/repro_1_3_proxy.py`.

**Mechanism:** `tcp_connect` (`src/pose64-mcp-proxy.cpp:54`) sets no `SO_RCVTIMEO`, so
`tcp_recv_line` (99) blocks forever on a wedged server. `rc_command`/`rc_command_multi`
(167-169, 185-187) reconnect **and re-send** on any read failure — double-executing
non-idempotent commands (`install`, `key`, `type`, `poke`, `delete`, and mutating
`tap`/`pen`/`button`/`menu`/`run`/`launch`/`reset`/`save`/`load`).

**Files:**
- Modify: `src/pose64-mcp-proxy.cpp`
- Test: `tests/phase1/repro_1_3_proxy.py` (uses a fake TCP server; the proxy binary is driven
  via stdin JSON-RPC `tools/call`)

- [x] **Step 1: Failing reproduction.** Fake server #1 accepts then never replies → proxy must
  return a structured timeout within the deadline, not hang. Fake server #2 accepts an
  `install`, reads one line, closes the socket → on the proxy's reconnect the command must NOT
  be re-sent (server #2 asserts it sees the command exactly once). Drive the proxy by writing a
  JSON-RPC `initialize` + `tools/call` to its stdin and reading stdout.

```python
#!/usr/bin/env python3
"""Repro 1.3: proxy must (a) time out on a silent server, (b) never re-send a
non-idempotent command after a mid-command disconnect."""
import json, os, socket, subprocess, sys, threading, time
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROXY = os.path.join(REPO, "build", "pose64-mcp-proxy")

def rpc(proxy, method, params, _id):
    msg = json.dumps({"jsonrpc":"2.0","id":_id,"method":method,"params":params}) + "\n"
    proxy.stdin.write(msg.encode()); proxy.stdin.flush()

def test_no_double_execute():
    seen = []
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0)); port = srv.getsockname()[1]; srv.listen(4)
    def serve():
        while True:
            conn, _ = srv.accept()
            data = conn.recv(4096)
            if data: seen.append(data)
            conn.close()  # drop immediately, mid-command
    threading.Thread(target=serve, daemon=True).start()
    proxy = subprocess.Popen([PROXY, "--host", "127.0.0.1", "--port", str(port)],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    rpc(proxy, "initialize", {}, 1); time.sleep(0.2)
    rpc(proxy, "tools/call", {"name":"palm_install","arguments":{"path":"/tmp/x.prc"}}, 2)
    time.sleep(1.0); proxy.terminate()
    installs = [d for d in seen if b"install" in d]
    assert len(installs) <= 1, f"install re-sent {len(installs)}x (double-execute!)"
    print("PASS 1.3 no-double-execute")
# test_timeout(): similar, server accepts and sleeps; assert proxy returns an error result
# (isError true / 'timeout' text) within ~deadline instead of hanging.
if __name__ == "__main__":
    test_no_double_execute()
```

  > Execution note: confirm the proxy's CLI flags (`--host/--port`) and the exact tool/arg
  > names from `pose64-mcp-proxy.cpp`. Finalize the timeout sub-test once the deadline value
  > is chosen in Step 3.

- [x] **Step 2: Run, confirm FAIL** (install seen ≥2×, or timeout test hangs).

- [x] **Step 3: Fix.** In `tcp_connect`, after `connect()` set a receive timeout:
```cpp
	struct timeval tv; tv.tv_sec = 30; tv.tv_usec = 0;   // > server's max command time
	setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
```
  Teach `tcp_recv_line` to distinguish timeout (`errno==EAGAIN||EWOULDBLOCK`) from EOF, e.g.
  return an `int` (1 ok / 0 lost / -1 timeout) and propagate. In `rc_command` and
  `rc_command_multi`, do NOT reconnect-and-resend for non-idempotent verbs:
```cpp
	static const std::set<std::string> kNonIdempotent = {
		"install","launch","key","type","poke","delete","tap","pen",
		"button","menu","run","reset","save","load"};
	// on timeout: return structured error, never retry
	// on connection-lost: retry ONLY if verb not in kNonIdempotent; otherwise
	//   return "ERR transient: connection lost; <verb> may or may not have run. "
	//   "Recovery: query state/apps before retrying."
```

- [x] **Step 4: Rebuild proxy.** `cmake --build build -j16 --target pose64-mcp-proxy` — exit 0.

- [x] **Step 5: Run repro, confirm PASS** (`PASS 1.3 no-double-execute` and the timeout test).

- [x] **Step 6: Commit (R5: SKILL.md + protocol doc note the proxy timeout/no-retry contract).**
```bash
git add tests/phase1/repro_1_3_proxy.py src/pose64-mcp-proxy.cpp docs/recontrol-protocol.md claude/skills/palm-dev/SKILL.md
git commit -m "fix: MCP proxy read timeout + no double-execute of non-idempotent commands (landmine #4)"
```

---

## Task 1.4: Bounded worker shutdown

**Mechanism:** `CPUWorkerThread::shutdown()` (`src/core/CPUWorkerThread.cpp:29`) queues a
`CMD_SHUTDOWN` then calls `wait()` **unbounded**; it never sets `fShouldStop`. If a slow/wedged
handler is in flight (e.g. a `kStopOnSysCall` mid-`ExecuteSubroutine`), the shutdown queues
behind it and the **main thread** blocks forever. Called from `ReControlCmds_Session.cpp:342`
(load/reset) and `src/ui/main.cpp:137` (exit).

**Files:**
- Modify: `src/core/CPUWorkerThread.cpp` (`shutdown`), `src/core/CPUWorkerThread.h` (no API change expected)
- Test: `tests/phase1/repro_1_4_shutdown.py`

- [ ] **Step 1: Failing reproduction.** Queue a slow command, then `load` (triggers
  `gCPUWorker->shutdown()`), assert the `load` returns within a bounded time and the server is
  still responsive.

```python
#!/usr/bin/env python3
"""Repro 1.4: palm_load while a slow command is in flight must not hang the main thread."""
import sys, time, threading
from _harness import emulator
from test_recontrol import ReControlClient
PORT = 6434
def main():
    with emulator(PORT):
        # client A issues something slow / wedged on the worker
        a = ReControlClient(port=PORT, timeout=20); a.connect()
        _start_slow_command(a)   # e.g. a command that wedges a stop (pre-1.2 wedge)
        time.sleep(0.3)
        b = ReControlClient(port=PORT, timeout=15); b.connect()
        t0 = time.time()
        resp = b.send_command("load freshm515.psf")
        dt = time.time() - t0
        assert resp.startswith("OK") or resp.startswith("ERR"), f"no response: {resp!r}"
        assert dt < 12.0, f"load blocked on shutdown for {dt:.1f}s"
        assert b.send_command("state").startswith("OK"), "server dead after load"
        print("PASS 1.4"); a.disconnect(); b.disconnect()
if __name__ == "__main__": main()
```

- [ ] **Step 2: Run, confirm FAIL** (`load` blocks > bounded window).

- [ ] **Step 3: Fix.** Rewrite `shutdown()`:
```cpp
void CPUWorkerThread::shutdown()
{
    {
        QMutexLocker locker(&fMutex);
        fShouldStop = true;
    }
    // Also enqueue an explicit sentinel so a thread parked in dequeueCommand
    // returns immediately even if it missed the flag.
    Command stopCmd; stopCmd.type = CMD_SHUTDOWN;
    stopCmd.handler = nullptr; stopCmd.response = nullptr;
    queueCommand(stopCmd);

    // Bounded wait: a wedged in-flight handler must not hang the main thread.
    // After 1.2, any EmSessionStopper inside the handler self-releases within
    // its own deadline, so 8s comfortably exceeds the worst legitimate case.
    if (!wait(8000)) {
        fprintf(stderr, "[CPUWorker] handler did not exit in 8s; terminating\n");
        terminate();   // last resort
        wait(2000);
    }
}
```
  Confirm `dequeueCommand` already honors `fShouldStop` (it does: 45-52). No header change.

- [ ] **Step 4: Rebuild + run repro.** `cmake --build build -j16` then
  `python3 repro_1_4_shutdown.py` — Expected: `PASS 1.4`.

- [ ] **Step 5: Commit.**
```bash
git add tests/phase1/repro_1_4_shutdown.py src/core/CPUWorkerThread.cpp docs/architecture.md
git commit -m "fix: bounded CPUWorkerThread::shutdown so load/reset/exit can't hang main thread"
```

---

## Task 1.5: Single ROM-call owner (close the two-stoppers window) — DANGEROUS

**Mechanism:** `EmSession::ExecuteSubroutine` (`EmSession.cpp:1238`) drops `fSharedLock`
(`omni_mutex_unlock unlock` at 1270) before `CallCPU()` (1273), so the 68K core runs unlocked.
Landmine #5: a second `SuspendThread(kStopOnSysCall)` can succeed trivially (979-986) while the
first owner is mid-core, and a GUI menu ROM call concurrent with a worker ROM call can both
drive UAE's global `regs`.

- [ ] **Step 1: Reproduce under TSAN FIRST.** Build `build-tsan`. Author an adversarial repro
  (`tests/phase1/repro_1_5_romcall_race.py`) that interleaves a GUI-equivalent ROM-call path
  with worker commands (`launch`/`tap-id`/`menu`) under TSAN and captures the data-race report
  on `regs`/UAE globals. Do NOT write fix code until the race is observed in a report.

- [ ] **Step 2: Design the owner.** Add a single global recursive ownership primitive entered
  immediately around `CallCPU()` in both `Run()` and `ExecuteSubroutine`. CRITICAL ordering:
  it must be acquired only while `fSharedLock` is NOT held (ExecuteSubroutine unlocks at 1270
  before `CallCPU` — acquire there), to avoid an A-B/B-A deadlock with `fSharedLock`. Prefer an
  owner-token check (assert single owner) + serialization over a blunt mutex if the blunt mutex
  risks deadlocking the suspend handshake. Record the chosen design in `docs/architecture.md`.

- [ ] **Step 3-6:** repro fails under TSAN → implement → TSAN clean → commit with the
  architecture note. **If a verified, deadlock-free fix cannot be achieved in this sitting,
  commit the TSAN reproduction + analysis and STOP (do not ship an unverified threading
  change — R4/honesty). Mark 1.5 as the next session's single task.**

> Rationale: this is the precise failure class that killed the project (symptom-layer
> threading fixes). Reproduce-first is non-negotiable here; an unverified core-mutex change
> is worse than a documented, reproduced, unfixed race.

---

## Task 1.6: PaintScreen screen-read safety — DANGEROUS

**Mechanism:** `EmWindow::PaintScreen` (`EmWindow.cpp:474`) calls `EmScreen::GetBits` (545)
with NO CPU stop and a comment claiming "a torn read is acceptable" (487). The identical call
in `GetLCDContents` (462) IS wrapped in `EmSessionStopper(gSession, kStopNow)` (457). `GetBits`
swaps the global `gMemAccessFlags` (via `CEnableFullAccess`) while the CPU runs on another
thread — not a torn pixel, a global-flag race. The 488-489 comment says the stopper was
*removed here* to avoid a nested-subroutine `SuspendThread` deadlock.

- [ ] **Step 1: Reproduce BOTH.** (a) The race: run the GATE-1 "concurrent screenshot+menu"
  scenario under TSAN and capture a `gMemAccessFlags` report. (b) The historical deadlock:
  re-add `EmSessionStopper(gSession, kStopNow)` around 545 on a scratch branch and drive
  PaintScreen while the CPU is nested in `ExecuteSubroutine` (e.g. during `load`/`launch`);
  observe whether it deadlocks. This decides the fix.

- [ ] **Step 2: Choose the fix.**
  - If `kStopNow` no longer deadlocks (1.1/1.2 changed the suspend handshake): wrap `GetBits`
    in PaintScreen exactly like `GetLCDContents` (the original POSE behavior).
  - If it still deadlocks: make `CEnableFullAccess`/`gMemAccessFlags` thread-safe for the
    read path instead (snapshot under a short lock, or a dedicated screen-read access mode that
    doesn't mutate the shared global). Record the decision in `docs/architecture.md` (#6).

- [ ] **Step 3-6:** repro fails under TSAN → implement chosen fix → TSAN clean + no deadlock
  under the nested-call drive → commit (update `docs/STATUS.md` landmine #6, `docs/architecture.md`
  Screen section, remove the misleading "torn read is acceptable" comment — R2/R5). Same STOP
  rule as 1.5 if not verifiable this sitting.

---

## Task 1.7: `fLastPenEvent` data race

**Mechanism:** `EmSession::PostPenEvent` (`EmSession.cpp:1907`) reads (1915) and writes (1926)
the non-atomic `fLastPenEvent` with no lock. It is called from the main thread (Qt mouse) and
the worker thread (`tap`/`pen` are `kCmdWorkerDirect`) → data race.

**Files:** `src/core/EmSession.cpp` (`PostPenEvent`), `src/core/EmSession.h` (new lock member).
**Test:** TSAN (`tests/phase1/repro_1_7_penrace.py`).

- [ ] **Step 1: Reproduce under TSAN.** Under `build-tsan`, drive rapid worker `tap`s while a
  second path posts pen events (the GATE-1 rapid-taps + a concurrent client) and capture the
  TSAN report naming `fLastPenEvent`.

- [ ] **Step 2: Fix.** Add a member to `EmSession.h` near the pen queue:
```cpp
	omni_mutex			fPenEventLock;	// guards fLastPenEvent across writer threads
```
  Serialize the critical section in `PostPenEvent`:
```cpp
void EmSession::PostPenEvent (const EmPenEvent& event)
{
	if (!::PrvCanBotherCPU())
		return;

	omni_mutex_lock	lock (fPenEventLock);

	if (event.fPenIsDown && event == fLastPenEvent)
		return;

	fPenQueue.Put (event);
	fLastPenEvent = event;
}
```

- [ ] **Step 3: Rebuild TSAN + confirm clean.** Re-run the repro; no `fLastPenEvent` report.
  Also rebuild `build/` and confirm functional taps still land (effect: screen-hash changes).

- [ ] **Step 4: Commit.**
```bash
git add tests/phase1/repro_1_7_penrace.py src/core/EmSession.cpp src/core/EmSession.h docs/architecture.md
git commit -m "fix: protect fLastPenEvent with a mutex (two-writer data race)"
```

---

## Task 1.8: WorkerDirect argument validation on the main thread

> **STATUS: DONE & VERIFIED 2026-06-10, commit `5f5c443`.**
> Repro: `tests/phase1/repro_1_8_argval.py`.

**Mechanism:** `QueueWork` (`ReControl.cpp:195`) hardcodes the `OK\n` response (211); the
`kCmdWorkerDirect` dispatch (439-448) discards the handler's return string. So `tap banana`
returns `OK` (arg-count error swallowed) and `tap 5 banana` returns `OK` with `y` silently
parsed as 0 by `QString::toInt`.

**Files:**
- Modify: `src/core/ReControl.h` (`RcCommandEntry` gains `validate`), `src/core/ReControl.cpp`
  (table entries for direct commands + dispatch), `src/core/ReControlCmds_Input.cpp` (validators)
- Test: `tests/phase1/repro_1_8_argval.py`

- [x] **Step 1: Failing reproduction.**
```python
#!/usr/bin/env python3
"""Repro 1.8: malformed WorkerDirect commands must return ERR usage, not OK."""
import sys
from _harness import emulator
from test_recontrol import ReControlClient
PORT = 6438
def main():
    with emulator(PORT):
        c = ReControlClient(port=PORT, timeout=5); c.connect()
        for bad in ["tap banana", "tap 5 banana", "tap", "key", "pen down 5"]:
            r = c.send_command(bad)
            assert r.startswith("ERR usage"), f"{bad!r} -> {r!r} (expected ERR usage)"
        # well-formed still works AND lands (effect):
        h1 = c.send_command("screen-hash")
        assert c.send_command("tap 80 80").strip() == "OK"
        # (effect assertion done in GATE 1; here just confirm OK on valid input)
        print("PASS 1.8"); c.disconnect()
if __name__ == "__main__": main()
```

- [x] **Step 2: Run, confirm FAIL** (`tap banana -> OK`).

- [x] **Step 3: Fix — add `validate` to the entry struct.** In `ReControl.h`, give
  `RcCommandEntry` a defaulted member so existing positional initializers still compile:
```cpp
	std::string (*validate)(const QStringList&) = nullptr;  // main-thread arg check, "" == ok
```
  Add validators in `ReControlCmds_Input.cpp`:
```cpp
std::string RcValidate_Tap (const QStringList& a)
{
	if (a.size () != 3) return "ERR usage: tap <x> <y>\n";
	bool ox=false, oy=false; a[1].toInt (&ox); a[2].toInt (&oy);
	if (!ox || !oy) return "ERR usage: tap <x> <y> (integers)\n";
	return "";
}
// RcValidate_Pen, RcValidate_Key, RcValidate_Type, RcValidate_Button similarly.
```
  Wire them into the table (`ReControl.cpp:116-121`) as the new field, and validate in the
  `kCmdWorkerDirect` dispatch BEFORE queueing:
```cpp
		case kCmdWorkerDirect:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			if (entry->validate) {
				std::string verr = entry->validate (parts);
				if (!verr.empty ()) { Send (verr); return; }
			}
			auto handler = entry->handler;
			QueueWork ([handler, parts]() { if (!gSession) return; handler (parts); });
			break;
		}
```

- [x] **Step 4: Rebuild + run repro.** `cmake --build build -j16` then
  `python3 repro_1_8_argval.py` — Expected: `PASS 1.8`.

- [x] **Step 5: Commit (R5: protocol doc — direct commands now validate args on the main thread).**
```bash
git add tests/phase1/repro_1_8_argval.py src/core/ReControl.h src/core/ReControl.cpp src/core/ReControlCmds_Input.cpp docs/recontrol-protocol.md
git commit -m "fix: validate WorkerDirect args on the main thread (no more swallowed ERR usage)"
```

---

## GATE 1

**Files:** `test_recontrol_stress.py` (add scenarios), `docs/STATUS.md`.

- [ ] **Step 1: Add effect-based scenarios** to `test_recontrol_stress.py`:
  - `commands_while_dialog_pending` — raise a dialog, fire `ui`/`tap-id`/`peek`, dismiss, assert
    `state`==running and a subsequent tap changes screen-hash (R3).
  - `load_during_queue` — slow command in flight + `load`, assert bounded response + alive.
  - `disconnect_storm` — 100 connect/abort cycles, assert server alive + responsive.
  - `two_client_busy_cycle` — second client gets `ERR busy`; alternate which client holds the
    session 50× without wedging.
  - `concurrent_screenshot_menu` — interleave `screenshot` and `menu` rapidly (drives 1.6 path).
- [ ] **Step 2: 30-minute ASAN soak.** Build `build-asan`; run the extended stress loop against
  it for 30 min. Expected: 0 ASAN reports, 0 hangs, process never restarts.
- [ ] **Step 3: TSAN pass.** Build `build-tsan`; run the suite. Expected: 0 TSAN reports with
  the new lock discipline (1.5/1.7 land here).
- [ ] **Step 4: Update `docs/STATUS.md`** — move every fixed landmine to "what works", with the
  verifying repro named. ONLY verified statements (R5).
- [ ] **Step 5: Tag + push** (only if the gate genuinely passes):
```bash
git tag -a phase-1-complete -m "GATE 1: 30-min ASAN soak clean, TSAN clean, effect-based"
git push origin master --tags
```

> Honesty gate: if 1.5 and/or 1.6 are reproduced-but-unfixed this session, GATE 1 is NOT
> complete. Push the verified per-task commits, tag them individually if useful, but do NOT
> apply the `phase-1-complete` tag until the soak+TSAN actually pass. Report exact status.

---

## Self-review (writing-plans)

- **Spec coverage:** recovery-plan tasks 1.1-1.8 each map to a task above; GATE 1 scenarios
  match the recovery plan's GATE 1 list. ✓
- **Placeholders:** the two DANGEROUS tasks (1.5/1.6) intentionally defer exact fix code to
  post-reproduction — this is correct discipline, not a placeholder, and each has a concrete
  reproduce-first Step 1 and a STOP rule. High-confidence tasks (1.1, 1.2, 1.3, 1.4, 1.7, 1.8)
  carry real fix code. ✓
- **Type/name consistency:** `fSuspendByUIThread`, `fPenEventLock`, `RcCommandEntry::validate`,
  `RcValidate_*` used consistently. ✓
- **Ordering:** 1.1 before 1.2 (shared function; leak fix is prerequisite for the timeout
  path's decrement). 1.2 before 1.4 (graceful shutdown escalation relies on stop timeouts).
  1.0 harness before all. ✓
