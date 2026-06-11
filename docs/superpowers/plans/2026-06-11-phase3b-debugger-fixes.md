# Phase 3b — Debugger Fixes Implementation Plan (SLP defuse + break-real)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Defuse the SLP debugger-socket trap (landmine #8) and make `break` real — a breakpoint hit with no external debugger raises the same Continue/Debug/Reset dialog watchpoints use (landmine #1).

**Architecture:** C2 first (small, de-risks C1's environment): gate `Debug::CreateListeningSockets` behind a new default-false pref + `--slp-debugger` CLI flag, and bound the two `EmSessionStopper` calls in `Debug::EventCallback` with the task-1.2 5000ms pattern. Then C1: a new `EmDeferredErrBreakpoint` modeled byte-for-byte on `EmDeferredErrWatchpoint`, scheduled from `Debug::EnterDebugger`'s no-debugger fallback — riding the 1.0d-hardened dialog state machine; resume via the existing `dialog respond continue`.

**Tech Stack:** C++ (core emulator: DebugMgr, ErrorHandling, PreferenceMgr), Qt6 main, Python 3 repro tests on `tests/lib`.

**Spec:** `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`
**Prerequisite:** Plan 3a complete (needs `tests/lib/harness.py` with `extra_args`, and the `palm_break` MCP tool whose description this plan flips).
**Sequence:** plan 2 of 3 (3a → **3b** → 3c).

---

## File map

| File | Action | Role |
|---|---|---|
| `tests/phase3/repro_slp_trap.py` | Create | C2 repro/regression test |
| `tests/phase3/test_break_real.py` | Create | C1 repro-style test (3× loop) |
| `src/core/PreferenceMgr.h` | Modify (:366 area) | new `SLPDebugger` bool pref, default false |
| `src/core/DebugMgr.h` | Modify | declare `Debug::ForceSocketsThisRun` |
| `src/core/DebugMgr.cpp` | Modify | socket gating (:1937), stopper timeouts (EventCallback ~:2055/:2090), EnterDebugger fallback (:1242-1245), index helper |
| `src/ui/main.cpp` | Modify (:56-75) | parse `--slp-debugger` |
| `src/core/Strings.r.h` | Modify (:97 area) | `kStr_ErrBreakpoint 1073` |
| `src/platform/ResStrings.cpp` | Modify (:111 area) | breakpoint dialog message |
| `src/core/ErrorHandling.h` | Modify | `ReportErrBreakpoint` decl + `EmDeferredErrBreakpoint` class |
| `src/core/ErrorHandling.cpp` | Modify (:3389 area) | implementations |
| `src/core/Hordes.cpp` | Modify (:2374 area) | error-name case for gremlin logs |
| `docs/recontrol-protocol.md`, `claude/skills/palm-dev/SKILL.md`, `src/pose64-mcp-proxy.cpp` (palm_break description), `docs/STATUS.md`, `docs/architecture.md` | Modify | truthful docs in the same commit as each fix (R5) |

Line numbers are as of `phase-2-complete`+plan-3a; verify against the live file before editing (the named functions are the anchors, not the numbers).

Build: `cmake --build build -j$(nproc)` (full pose64 target — these are core changes).

---

### Task B1: defuse the SLP trap (recovery task 3.3, landmine #8)

- [ ] **Step 1: Write the repro/regression test**

`tests/phase3/repro_slp_trap.py`:

```python
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
```

- [ ] **Step 2: Run it on the unfixed build — expect RED at part 1**

```bash
python3 tests/phase3/repro_slp_trap.py
```

Expected: `AssertionError: port 6414 accepted a connection with SLP off (default)` — this is the R1 reproduction. (If 6414 is somehow not listening, STOP and investigate before changing code.)

- [ ] **Step 3: Add the pref and the force-flag**

`src/core/PreferenceMgr.h` — directly below the `DebuggerSocketPort` line (:366):

```cpp
	DO_TO_PREF(SLPDebugger,			bool,				(false))				\
```

`src/core/DebugMgr.h` — in `class Debug`'s public statics (near `Startup`):

```cpp
		static void 			ForceSocketsThisRun 	(void);
```

`src/core/DebugMgr.cpp` — near the other socket globals (:123 area):

```cpp
static Bool				gForceDebuggerSockets;	// --slp-debugger CLI flag (this run only,
												// never persisted to preferences)
```

and the setter (above `CreateListeningSockets`):

```cpp
void Debug::ForceSocketsThisRun (void)
{
	gForceDebuggerSockets = true;
}
```

- [ ] **Step 4: Gate socket creation**

In `Debug::CreateListeningSockets` (`DebugMgr.cpp:1937`), replace the body's opening with:

```cpp
	// Phase 3b (landmine #8): the SLP debugger sockets are OFF by default.
	// Opt in with the SLPDebugger preference or the --slp-debugger CLI flag.
	// (Off also disarms the EventCallback stop-on-connect path entirely.)
	Preference<bool>	enabledPref (kPrefKeySLPDebugger);
	Preference<long>	portPref (kPrefKeyDebuggerSocketPort);

	EmAssert (gDebuggerSocket1 == NULL);
	EmAssert (gDebuggerSocket2 == NULL);
	EmAssert (gDebuggerSocket3 == NULL);

	if (!*enabledPref && !gForceDebuggerSockets)
		return;

	if (*portPref != 0)
	{
		gDebuggerSocket1 = new CTCPSocket (&Debug::EventCallback, *portPref);
		gDebuggerSocket2 = new CTCPSocket (&Debug::EventCallback, 2000);
	}

	gDebuggerSocket3 = Platform::CreateDebuggerSocket ();
```

(rest unchanged — the `PrvFireUpSocket` calls. Note `gDebuggerSocket3` is `NULL` on Unix, `Platform_Unix.cpp:752`, so the early return is the complete story here.)

- [ ] **Step 5: Bound the EventCallback stoppers (task-1.2 pattern)**

In `Debug::EventCallback`, both stopper sites — `kConnected` (FtrSet) and `kDisconnected` (FtrUnregister) — change from:

```cpp
			if (EmPatchState::UIInitialized ())
			{
				EmSessionStopper	stopper (gSession, kStopOnSysCall);
				if (stopper.Stopped ())
				{
					::FtrSet ('gdbS', 0, 0x12BEEF34);
				}
			}
```

to:

```cpp
			if (EmPatchState::UIInitialized ())
			{
				// Bounded (Phase 3b): an unstoppable CPU must not wedge the
				// UI thread on a debugger connect (landmine #8).  The 'gdbS'
				// feature is best-effort; skipping it degrades prc-tools
				// auto-break, nothing else.
				EmSessionStopper	stopper (gSession, kStopOnSysCall, 5000);
				if (stopper.Stopped ())
				{
					::FtrSet ('gdbS', 0, 0x12BEEF34);
				}
				else
				{
					fprintf (stderr, "POSE64: SLP connect: CPU did not stop "
							 "within 5000ms; skipping gdbS feature set\n");
				}
			}
```

(and the mirror-image for `FtrUnregister` in `kDisconnected`, message "...skipping gdbS feature unregister").

- [ ] **Step 6: Parse `--slp-debugger` in main**

`src/ui/main.cpp` — add `#include "DebugMgr.h"` with the other core includes, and extend the pre-Qt arg loop (after the `--no-recontrol` branch, same strip-from-argv pattern):

```cpp
		else if (strcmp (argv[i], "--slp-debugger") == 0)
		{
			Debug::ForceSocketsThisRun ();
			for (int j = i; j < argc - 1; j++)
				argv[j] = argv[j + 1];
			argc -= 1;
			i--;
		}
```

- [ ] **Step 7: Grep-gate — no untimed stops left in EventCallback**

```bash
grep -n "EmSessionStopper" src/core/DebugMgr.cpp
```

Expected: every hit in `EventCallback` carries the `5000` argument.

- [ ] **Step 8: Build, run the repro 3×, run fast phase-1 repros**

```bash
cmake --build build -j$(nproc)
for i in 1 2 3; do python3 tests/phase3/repro_slp_trap.py || exit 1; done
python3 tests/phase1/repro_1_8_argval.py && python3 tests/phase1/repro_1_2_stop_timeout.py
```

Expected: `ALL PASS` ×3; phase-1 repros PASS.

- [ ] **Step 9: Docs in the same commit**

- `docs/recontrol-protocol.md`: in the Debugging section's `break` warning paragraph, update the SLP sentence: external Palm-Debugger attach now requires launching with `--slp-debugger` (or the `SLPDebugger` pref); ports 6414/2000 no longer listen by default.
- `docs/STATUS.md`: landmine #8 → `~~...~~ **FIXED (Phase 3b)**` with the mechanism (default-false `SLPDebugger` pref + `--slp-debugger` flag gate `CreateListeningSockets`; both `EventCallback` stoppers bounded at 5000ms with graceful skip). Repro: `tests/phase3/repro_slp_trap.py` 3× PASS.
- `docs/architecture.md`: if its threading/Do-Not-Do text mentions the SLP sockets or untimed stops (`grep -in "slp\|6414" docs/architecture.md`), update those lines to match.

- [ ] **Step 10: Commit**

```bash
git add src/core/PreferenceMgr.h src/core/DebugMgr.h src/core/DebugMgr.cpp \
        src/ui/main.cpp tests/phase3/repro_slp_trap.py \
        docs/recontrol-protocol.md docs/STATUS.md docs/architecture.md
git commit -m "fix(phase3b): SLP sockets off by default + bounded EventCallback stoppers (task 3.3, landmine #8)"
```

---

### Task B2: make `break` real (recovery task 3.2, landmine #1)

- [ ] **Step 1: Write the failing test**

`tests/phase3/test_break_real.py`:

```python
#!/usr/bin/env python3
"""Landmine #1: a breakpoint hit with no SLP debugger must raise the
Continue/Debug/Reset dialog (blocked_on_ui), be inspectable, and resume on
'dialog respond continue'.  3x loop per repro convention.

Strategy for a deterministic hit address: backtrace frame PCs belong to the
active event loop — they re-execute on the next delivered event.  We take a
mid-stack frame PC, set breakpoint 0 there, deliver a tap, and expect the hit.

Pre-fix: the guest never blocks (hit silently ignored) -> FAIL.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6427


def multiline(c, cmd):
    c.socket.sendall((cmd + "\n").encode("latin-1"))
    return c.read_multiline_response()


def wait_state(c, want, timeout=6.0):
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        last = c.send_command("state") or ""
        if want in last:
            return last
        time.sleep(0.25)
    return last


def one_round(c, rnd):
    bt = multiline(c, "backtrace")
    pcs = re.findall(r"PC=([0-9A-Fa-fx]+)", bt)
    assert len(pcs) >= 2, f"backtrace too shallow: {bt!r}"
    addr = pcs[1] if pcs[1].startswith("0x") else "0x" + pcs[1]

    r = c.send_command(f"break set 0 {addr}")
    assert r.startswith("OK"), f"break set failed: {r}"

    r = c.send_command("tap 80 80")  # drive the event loop through the frame
    assert r.startswith("OK") or "pending" in r, f"tap failed: {r}"

    state = wait_state(c, "blocked_on_ui")
    assert "blocked_on_ui" in state, f"round {rnd}: no dialog stop (state={state!r})"

    dlg = multiline(c, "dialog")
    assert "reakpoint" in dlg, f"round {rnd}: dialog is not the breakpoint dialog: {dlg!r}"
    assert addr.lower().replace("0x", "") in dlg.lower().replace("0x", ""), \
        f"round {rnd}: dialog lacks hit address {addr}: {dlg!r}"
    assert "regs" in dlg or "PC=" in dlg, f"round {rnd}: dialog lacks register dump: {dlg!r}"

    bt2 = multiline(c, "backtrace")  # inspectable while stopped
    assert "PC=" in bt2, f"round {rnd}: backtrace unavailable while blocked: {bt2!r}"

    r = c.send_command("break clearall")  # avoid immediate re-hit on resume
    assert r.startswith("OK"), f"break clearall failed: {r}"

    r = c.send_command("dialog respond continue")
    assert r.startswith("OK"), f"continue failed: {r}"

    state = wait_state(c, "running")
    assert "running" in state, f"round {rnd}: did not resume: {state!r}"
    print(f"PASS round {rnd} (hit @ {addr})")


def main():
    with emulator(PORT):
        c = connect(PORT, timeout=10)
        try:
            for rnd in (1, 2, 3):
                one_round(c, rnd)
                time.sleep(0.5)
        finally:
            c.disconnect()
    print("ALL PASS")


if __name__ == "__main__":
    main()
```

(Adjust `multiline()` to `ReControlClient`'s actual API if it differs — it exposes `send_command` and `read_multiline_response` per `docs/recontrol-protocol.md` §Python client.)

- [ ] **Step 2: Run it — expect RED**

```bash
python3 tests/phase3/test_break_real.py
```

Expected: `round 1: no dialog stop (state='OK running')` — the silent-continue landmine, reproduced.

- [ ] **Step 3: Add the string resource**

`src/core/Strings.r.h` after `kStr_ErrMemoryLeaks` (:97):

```cpp
#define kStr_ErrBreakpoint				1073
```

`src/platform/ResStrings.cpp` after the `kStr_ErrWatchpoint` entry (:111):

```cpp
	{ kStr_ErrBreakpoint, "%App hit breakpoint %bp_index at address %bp_addr. \"Continue\" resumes execution." },
```

- [ ] **Step 4: Declare the deferred error**

`src/core/ErrorHandling.h` — in `class Errors`, next to `ReportErrStorageHeap` (:108 area):

```cpp
		static void				ReportErrBreakpoint			(int index, emuptr pc);
```

and after `class EmDeferredErrWatchpoint` (:496-512), before the closing `#endif`:

```cpp
class EmDeferredErrBreakpoint : public EmDeferredErr
{
	public:
								EmDeferredErrBreakpoint			(int index, emuptr pc);
		virtual					~EmDeferredErrBreakpoint		(void);

		virtual void			Do								(void);

	protected:
		int						fIndex;
		emuptr					fPC;
};
```

- [ ] **Step 5: Implement, modeled on the watchpoint twin**

`src/core/ErrorHandling.cpp` — after `EmDeferredErrWatchpoint::Do` (:3389), and a `ReportErrBreakpoint` next to `ReportErrWatchpoint` (:1735 area):

```cpp
// ---------------------------------------------------------------------------
//		� Errors::ReportErrBreakpoint
// ---------------------------------------------------------------------------
// A breakpoint (or guest DbgBreak) fired with no external SLP debugger
// attached.  Phase 3b: surface it as the same Continue/Debug/Reset dialog
// watchpoints use instead of silently resuming (landmine #1).

void Errors::ReportErrBreakpoint (int index, emuptr pc)
{
	// Set the %app message variable.

	Errors::SetStandardParameters ();

	// Set the %bp_index and %bp_addr message variables.

	string	indexStr = index >= 0 ? ::PrvAsDecimal (index) : string ("?");
	Errors::SetParameter ("%bp_index", indexStr.c_str ());

	string	pcStr (::PrvAsHex8 (pc));
	Errors::SetParameter ("%bp_addr", pcStr.c_str ());

	// Show the dialog.

	Errors::HandleDialog (kStr_ErrBreakpoint, kException_SoftBreak,
			kDlgFlags_Continue_DEBUG_Reset, false);
}
```

```cpp
#pragma mark -

// ---------------------------------------------------------------------------
//		� EmDeferredErrBreakpoint
// ---------------------------------------------------------------------------

EmDeferredErrBreakpoint::EmDeferredErrBreakpoint (int index, emuptr pc) :
	EmDeferredErr (),
	fIndex (index),
	fPC (pc)
{
}

EmDeferredErrBreakpoint::~EmDeferredErrBreakpoint (void)
{
}

void EmDeferredErrBreakpoint::Do (void)
{
	Errors::ReportErrBreakpoint (fIndex, fPC);
}
```

(If `PrvAsDecimal`/`PrvAsHex8` are file-static and not visible at the insertion point, place `ReportErrBreakpoint` adjacent to `ReportErrWatchpoint` where they demonstrably are.)

- [ ] **Step 6: Schedule it from the EnterDebugger fallback**

`src/core/DebugMgr.cpp` — add a file-static helper above `EnterDebugger` (:1176):

```cpp
// Find which enabled breakpoint slot covers the given PC (dialog text only).
// -1 when none matches (e.g. an explicit DbgBreak from guest code, or an
// A-Trap break).

static int PrvBreakpointIndexForPC (emuptr pc)
{
	for (int ii = 0; ii < dbgTotalBreakpoints; ++ii)
	{
		if (gDebuggerGlobals.bp[ii].enabled &&
			gDebuggerGlobals.bp[ii].addr == (MemPtr)(uintptr_t) pc)
		{
			return ii;
		}
	}

	return -1;
}
```

and replace `EnterDebugger`'s failure branch (:1242-1245):

```cpp
	else
	{
		PRINTF ("Failed to enter debug mode.");
	}
```

with:

```cpp
	else
	{
		// No external SLP debugger is attached.  Pre-Phase-3 the hit was
		// silently ignored here (landmine #1).  Surface it instead as the
		// same deferred-error dialog watch/spy use: the CPU blocks on a
		// Continue/Debug/Reset dialog (blocked_on_ui) that ReControl's
		// `dialog` / `dialog respond continue` can drive.  Scheduling is
		// CPU-thread-safe — identical to Debug::DoCheckWatchpoint.

		emuptr	pc = m68k_getpc ();

		EmAssert (gSession);
		gSession->ScheduleDeferredError (
				new EmDeferredErrBreakpoint (::PrvBreakpointIndexForPC (pc), pc));

		PRINTF ("No debugger attached; scheduled breakpoint dialog.");
	}
```

Callers stay correct: `ConditionalBreak` (:1907) ignores the result ("just leave" — now with a dialog queued); `HandleSystemCall` (:1080) only enters this path when an SLP debugger had set A-Trap breaks, and its `!= kError_NoError` branch behavior is unchanged. The `&slp` call site (:646) never reaches the `!slp` fallback.

- [ ] **Step 7: Gremlin error-name mapping**

`src/core/Hordes.cpp` — in the error-name switch after `case kStr_ErrWatchpoint:` (:2374):

```cpp
		case kStr_ErrBreakpoint:
			return "ErrBreakpoint";
			break;
```

- [ ] **Step 8: Build and run the test 3×**

```bash
cmake --build build -j$(nproc)
for i in 1 2 3; do python3 tests/phase3/test_break_real.py || exit 1; done
```

Expected: `ALL PASS` ×3 (each run is itself 3 rounds). If a round flakes on the frame-PC strategy (event loop not re-reaching the frame), pick `pcs[2]` instead and note it in the test — do NOT loosen the blocked_on_ui assertion.

- [ ] **Step 9: TSAN spot-check + phase-1/2 regression**

```bash
cmake --build build-tsan -j$(nproc)
python3 - <<'EOF'
import sys; sys.path.insert(0, ".")
# one TSAN pass of the break flow, reusing the test against the TSAN binary
from tests.lib import harness
import tests.phase3.test_break_real as t
orig = harness.emulator
def tsan_emulator(port, **kw):
    kw["build"] = "build-tsan"
    kw.setdefault("capture_log", "/tmp/tsan_break.log")
    return orig(port, **kw)
harness.emulator = tsan_emulator
t.emulator = tsan_emulator
t.main()
EOF
grep -c "WARNING: ThreadSanitizer" /tmp/tsan_break.log || true
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
python3 tests/phase2/test_honest_ack.py
```

Expected: break flow passes under TSAN; any TSAN reports are the documented #5/#6 baseline families only (inspect `/tmp/tsan_break.log` — zero reports implicating `EmDeferredErrBreakpoint`/`ReportErrBreakpoint`/`ScheduleDeferredError` from the new path); phase-1 repros 7/7; honest-ACK PASS.

- [ ] **Step 10: Flip every description and doc in the same commit (R5)**

- `src/pose64-mcp-proxy.cpp` — `palm_break` description becomes:

```
"Manage the 6 m68k breakpoint slots. action=set requires idx+addr (condition "
"optional, e.g. 'd0 == 0'); clear/enable/disable require idx. On hit the CPU "
"blocks on a Continue/Debug/Reset dialog (state=blocked_on_ui): inspect with "
"palm_dialog/palm_backtrace/palm_peek, resume with palm_dialog "
"respond=continue. With an external SLP debugger attached (--slp-debugger), "
"the debugger takes the hit instead."
```

  Rebuild the proxy: `cmake --build build --target pose64-mcp-proxy -j$(nproc)`.
- `docs/recontrol-protocol.md` — replace the "**`break` does not stop execution on its own.**" warning block with the new contract: hit → Continue/Debug/Reset dialog → `blocked_on_ui` → `dialog`/`dialog respond continue`; SLP attach (opt-in) takes precedence.
- `claude/skills/palm-dev/SKILL.md` — update the Debugging workflows section: breakpoint flow is now set → trigger → poll `palm_state` for `blocked_on_ui` → `palm_dialog`/`palm_backtrace` → `palm_break action=clearall` → `palm_dialog respond=continue`.
- `docs/STATUS.md` — landmine #1 → `~~...~~ **FIXED (Phase 3b)**` with mechanism (EnterDebugger fallback schedules `EmDeferredErrBreakpoint` → `Errors::HandleDialog`, same path as watchpoints) and evidence (`tests/phase3/test_break_real.py` 3× PASS, TSAN spot-check clean).
- `claude/agents/pose64-tester.md` — breakpoint capability note.

- [ ] **Step 11: Verify drift gate still green, commit**

```bash
python3 tests/phase3/test_mcp_surface.py
git add src/core/Strings.r.h src/platform/ResStrings.cpp src/core/ErrorHandling.h \
        src/core/ErrorHandling.cpp src/core/DebugMgr.cpp src/core/Hordes.cpp \
        src/pose64-mcp-proxy.cpp tests/phase3/test_break_real.py \
        docs/recontrol-protocol.md claude/skills/palm-dev/SKILL.md docs/STATUS.md \
        claude/agents/pose64-tester.md
git commit -m "feat(phase3b): break is real — hit raises Continue/Debug/Reset dialog (task 3.2, landmine #1)"
```

---

### Task B3: Plan 3b close-out

- [ ] **Step 1: Recovery-plan banner**

`docs/recovery-plan-2026-06.md`: CURRENT POSITION → plan 3b DONE (tasks 3.2 + 3.3 fixed with repros); NEXT = plan 3c (`2026-06-11-phase3c-metamemory-gate3.md`: landmine #7 root fix + GATE 3). Annotate 3.2/3.3 in the Phase 3 section as done.

- [ ] **Step 2: Full sweep and commit**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
python3 tests/phase3/repro_slp_trap.py && python3 tests/phase3/test_break_real.py
python3 tests/phase3/test_mcp_surface.py && python3 tests/phase3/test_mcp_dispatch.py
git add docs/recovery-plan-2026-06.md
git commit -m "docs(phase3b): banner — 3.2/3.3 done; next plan 3c (MetaMemory + GATE 3)"
```

Expected: everything PASS before the commit lands.
