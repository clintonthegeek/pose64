# Task 1.0d — Dialog-Action Lifetime Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to
> implement this plan inline (R6: this is a cross-thread lifetime fix — strongest
> model, single focused sitting, do NOT delegate the C++ work to weaker
> subagents). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `reset` (or session stop) while a deferred-error dialog is queued or
showing must never crash the process; after the fix, raising and dismissing
spy/watch/error dialogs is safe, which unblocks the 1.1/1.2 reproductions.

**Architecture:** Fix the lifetime contract in `EmSession::BlockOnDialog`: the
queued `EmActionDialog` references CPU-thread stack data (`parameters`, the
`result` local), so `BlockOnDialog` must not return until that action either
(a) is flagged cancelled while still queued — it will then never touch the
data — or (b) has finished running. A small state machine in `EmSession`
guarded by the existing `fSharedLock` implements this; `UnblockDialog` is
replaced (R2) by `BeginDialogAction`/`EndDialogAction`.

**Tech stack:** Qt6/C++17, omni_thread, ASAN, Python repro
(`tests/phase1/repro_dialog_subsystem.py`).

---

## Verified facts this plan is built on (live-tested 2026-06-10, HEAD `103356e`)

These were established by the plan designer with a live emulator (offscreen),
correcting the 2026-06-10 finding doc:

1. **The dialog SHOWS correctly.** After `spy set 0x134`, `state` reports
   `blocked_on_ui` at ~+50 ms and `dialog` reports the full dialog (message +
   buttons) at ~+100 ms — exactly one idle-timer tick (`main.cpp:113`, 100 ms).
   **"Bug A" (permanent hang, `dialog` → `none` for many seconds) did NOT
   reproduce** and is presumed to have been a mis-observation of the ≤100 ms
   queued-but-not-yet-shown window. A regression guard is added anyway (Step 2).
2. **`dialog respond continue` works.** The CPU resumes; the spy re-fires and
   the next dialog shows. The show→query→respond cycle is healthy.
3. **Crash interleaving #1 — reset while the action is QUEUED** (within the
   ≤100 ms window): `ForceReset` sets `fReset`, `BlockOnDialog`'s wait exits
   (`EmSession.cpp:1625`), the CPU thread unwinds and frees the stack-local
   `EditCommonDialogData` (`EmDlg.cpp:4709`) and `RunDialogParameters`
   (`EmDlg.cpp:5571`); the main thread later runs `EmActionDialog::Do`
   (`EmDocument.cpp:132`) → `QString::fromUtf8(dangling fMessage)` → **SIGSEGV**.
   Deterministic: `tests/phase1/repro_dialog_subsystem.py` exits on signal 11.
4. **Crash interleaving #2 — reset while the dialog is SHOWING**: `RcCmd_Reset`
   → `EmDlgQt_DismissIfPending` rejects the `QMessageBox`; `ForceReset` breaks
   the CPU out of `BlockOnDialog`; the CPU unwinds while the main thread is
   still inside `msgBox.exec()`; when `exec` returns, `Do` writes
   `fDlgResult` — a reference to the dead stack `result` — corrupting the CPU
   thread's live stack. Verified live: the process replies
   `OK reset (was blocked_on_ui, dialog dismissed)` and then **dies**.
5. Because the dialog subsystem otherwise works, **tasks 1.1/1.2 are NOT
   hang-blocked** — they were deferred on a false premise. They remain
   sequenced after this task only because their repros raise dialogs
   constantly and need `reset` to be a safe recovery.

## Out of scope (do not creep)

- `EmDlg::RunDialog` called from the CPU **worker** thread would run a
  QMessageBox off the GUI thread (`EmDlg.cpp:5596-5610` falls through to
  `fn (parameters)` for any non-CPU thread). Latent, unobserved; not this task.
- `EmActionHandler::DoAll`'s `DeleteAll()` on an `ErrCode` throw can orphan a
  queued dialog action without running it (pre-existing, exotic). The fix here
  keeps the CPU recoverable via `reset` in that case; no further handling.
- The 1.1 suspend-counter fix itself (its reverted diff lives in
  `2026-06-10-phase1-kill-freeze-classes.md` Task 1.1).

## File structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/core/EmSession.h` | dialog-action state enum + members; `BeginDialogAction`/`EndDialogAction`; delete `UnblockDialog` decl (line 439) | modify |
| `src/core/EmSession.cpp` | `BlockOnDialog` early-exit handshake (~1604-1668); new methods; delete `UnblockDialog` (~1672-1684); constructor init | modify |
| `src/core/EmDocument.h` | `ScheduleDialog` returns `EmAction*` | modify |
| `src/core/EmDocument.cpp` | `EmActionDialog::Do` brackets with Begin/End (132-142); `ScheduleDialog` (705-713) | modify |
| `tests/phase1/repro_dialog_subsystem.py` | extend: both interleavings + show-latency guard + respond cycle | modify |
| `docs/STATUS.md` | landmine #9 → fixed, verified wording | modify (same commit) |
| `docs/architecture.md` | record the dialog-action lifetime contract | modify (same commit) |

---

## Step-by-step

- [ ] **Step 1: Confirm the failing baseline.**

Run: `cd tests/phase1 && python3 repro_dialog_subsystem.py`
Expected: `AssertionError: DIALOG SUBSYSTEM BUG: emulator crashed (signal/exit -11) ...`
(If it passes, STOP — the world changed; re-diagnose before touching anything.)

- [ ] **Step 2: Extend the reproduction to cover both interleavings.**

Replace the body of `tests/phase1/repro_dialog_subsystem.py` `main()` with a
three-part test (keep the module docstring, update it to match):

```python
PORT = 6460
SPY_ADDR = "0x134"          # low-mem global written ~100x/s
SHOW_DEADLINE = 2.0         # dialog must show within this after blocked_on_ui


def wait_for(c, pred, timeout, interval=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(interval)
    return False


def get_blocked(c):
    assert (c.send_command("spy set " + SPY_ADDR) or "").startswith("OK")
    assert wait_for(c, lambda: "blocked_on_ui" in (c.send_command("state") or ""), 5.0), \
        "setup: never reached blocked_on_ui"


def dialog_active(c):
    return "message=" in (c.send_command("dialog") or "")


def assert_alive_and_running(proc, port, label):
    assert proc.poll() is None, f"{label}: emulator crashed (signal/exit {proc.poll()})"
    c = ReControlClient(port=port, timeout=5)
    assert c.connect(), f"{label}: emulator unresponsive (connect failed)"
    ok = wait_for(c, lambda: (c.send_command("state") or "").startswith("OK running"), 20.0, 0.5)
    assert ok, f"{label}: never returned to OK running after reset"
    c.disconnect()


def main():
    with emulator(PORT) as proc:
        c = ReControlClient(port=PORT, timeout=5)
        assert c.connect(), "connect failed"

        # --- Part 1: the dialog must actually show (hang regression guard) ---
        get_blocked(c)
        assert wait_for(c, lambda: dialog_active(c), SHOW_DEADLINE), \
            "HANG: dialog never became visible after blocked_on_ui"

        # --- Part 2: reset while the dialog is SHOWING (UAF write path) ---
        c.send_command("reset")
        c.disconnect()
        time.sleep(2.0)
        assert_alive_and_running(proc, PORT, "reset-while-showing")

        # --- Part 3: reset while the action is QUEUED (UAF read path).
        # Send reset as fast as possible after blocked_on_ui, repeatedly, to
        # land inside the <=100ms idle-tick window at least once.
        for i in range(4):
            c = ReControlClient(port=PORT, timeout=5)
            assert c.connect(), f"iter {i}: connect failed"
            get_blocked(c)
            c.send_command("reset")        # no dialog poll: race the idle tick
            c.disconnect()
            time.sleep(2.0)
            assert_alive_and_running(proc, PORT, f"reset-while-queued[{i}]")

        print("PASS - deferred-error dialogs survive reset in both interleavings")
```

Notes for the implementer:
- `_harness.py` and `ReControlClient` usage is unchanged from the current file.
- After a soft reset the spy may or may not persist — `get_blocked` re-issues
  `spy set` each iteration; if it returns `ERR` because a spy is already set,
  issue `spy clear` first and retry once.
- Keep the file's leading comment honest: it must describe the verified
  two-interleaving UAF, not the disproven "never shows" hang.

- [ ] **Step 3: Run the extended repro, confirm it still FAILS.**

Run: `cd tests/phase1 && python3 repro_dialog_subsystem.py`
Expected: crash assertion from Part 2 or Part 3 (either interleaving kills the
process today).

- [ ] **Step 4: Implement the lifetime fix — `EmSession.h`.**

Add to the public section of `EmSession` (near the `BlockOnDialog`
declaration), guarded like the existing `UnblockDialog`:

```cpp
#if HAS_OMNI_THREAD
		// Dialog-action lifetime handshake (see BlockOnDialog).  Called by
		// EmActionDialog::Do on the UI thread.  BeginDialogAction returns
		// false if the request was cancelled (the CPU stack the action's
		// parameters point into is gone) -- the action must then do nothing.
		Bool					BeginDialogAction	(EmAction* self);
		void					EndDialogAction		(void);
#endif
```

Delete the `UnblockDialog` declaration (`EmSession.h:439`).

Add to the private members (near the other `fSharedLock`-guarded state), with
the enum just above the class or in the private section:

```cpp
#if HAS_OMNI_THREAD
		enum EmDialogActionState
		{
			kDlgActionNone,			// no dialog action outstanding
			kDlgActionQueued,		// posted, not yet picked up by the UI thread
			kDlgActionRunning,		// UI thread is inside RunDialog
			kDlgActionDone,			// UI thread finished; result written
			kDlgActionCancelled		// CPU thread gave up while still queued
		};

		EmDialogActionState		fDialogActionState;	// guarded by fSharedLock
		EmAction*				fDialogAction;		// identity only -- never dereferenced
#endif
```

`#include` or forward-declare `class EmAction;` as needed (forward declaration
is enough — the pointer is only compared, never dereferenced).

- [ ] **Step 5: Implement the lifetime fix — `EmSession.cpp`.**

5a. Initialize the new members in the `EmSession` constructor's init list:

```cpp
	fDialogActionState (kDlgActionNone),
	fDialogAction (NULL),
```

(Place them with the other `#if HAS_OMNI_THREAD` members if the init list is
conditionalized; follow the existing pattern in the constructor.)

5b. Rewrite the `HAS_OMNI_THREAD` branch of `BlockOnDialog`
(`EmSession.cpp:1604-1641`):

```cpp
EmDlgItemID EmSession::BlockOnDialog (EmDlgThreadFn fn, const void* parameters)
{
#if HAS_OMNI_THREAD
	EmAssert (this->InCPUThread ());
	EmAssert (gDocument);

	omni_mutex_lock	lock (fSharedLock);

	EmDlgItemID	result = kDlgItemNone;

	fDialogActionState	= kDlgActionQueued;
	fDialogAction		= gDocument->ScheduleDialog (fn, parameters, result);

	{
		EmValueChanger<EmSessionState>	oldState (fState, kBlockedOnUI);

		// Broadcast the change in fState.

		fSharedCondition.broadcast ();

		while (result == kDlgItemNone && !fStop && !fReset)
		{
			EmAssert (fState == kBlockedOnUI);
			fSharedCondition.wait ();
		}

		// LIFETIME CONTRACT: the scheduled EmActionDialog references
		// `result` and `parameters`, which live on THIS thread's stack.
		// We must not return -- destroying that stack -- while the action
		// could still touch them.  Three cases:
		//   queued   -> flag it cancelled; BeginDialogAction will skip it
		//               and the action will never touch the dead data.
		//   running  -> the UI thread is inside RunDialog (msgBox.exec);
		//               wait here (stack stays alive) until EndDialogAction.
		//   done     -> nothing outstanding.

		if (fDialogActionState == kDlgActionQueued)
		{
			// fDialogAction stays set so the stale action can identify
			// itself in BeginDialogAction.  It is never dereferenced.
			fDialogActionState = kDlgActionCancelled;
		}
		else
		{
			while (fDialogActionState == kDlgActionRunning)
				fSharedCondition.wait ();

			fDialogActionState	= kDlgActionNone;
			fDialogAction		= NULL;
		}
	}

	// Broadcast the change in fState.

	fSharedCondition.broadcast ();

	return result;

#else
	... (non-omni branch unchanged) ...
#endif
}
```

5c. Replace `UnblockDialog` (`EmSession.cpp:1671-1684`) with the two handshake
methods — delete `UnblockDialog` entirely (R2):

```cpp
// ---------------------------------------------------------------------------
//		. EmSession::BeginDialogAction
// ---------------------------------------------------------------------------
// Called by EmActionDialog::Do (UI thread) before touching its parameters.
// Returns false if the dialog request was cancelled or superseded: the CPU
// stack frame the parameters point into no longer exists, so the action
// must return without touching them.

#if HAS_OMNI_THREAD
Bool EmSession::BeginDialogAction (EmAction* self)
{
	omni_mutex_lock	lock (fSharedLock);

	if (fDialogActionState != kDlgActionQueued || fDialogAction != self)
	{
		// Either BlockOnDialog gave up while we were queued (cancelled),
		// or we are a stale action from an earlier, cancelled request and
		// a NEWER request is now queued.  Only clear the state if it is
		// still ours to clear.

		if (fDialogAction == self)
		{
			fDialogActionState	= kDlgActionNone;
			fDialogAction		= NULL;
		}

		fSharedCondition.broadcast ();
		return false;
	}

	fDialogActionState = kDlgActionRunning;
	return true;
}


// ---------------------------------------------------------------------------
//		. EmSession::EndDialogAction
// ---------------------------------------------------------------------------
// Called by EmActionDialog::Do (UI thread) after RunDialog returns and the
// result has been written.  Wakes BlockOnDialog, which may be waiting for
// the action to finish before letting its stack frame die.

void EmSession::EndDialogAction (void)
{
	omni_mutex_lock	lock (fSharedLock);

	fDialogActionState	= kDlgActionDone;
	fDialogAction		= NULL;

	fSharedCondition.broadcast ();
}
#endif
```

- [ ] **Step 6: Implement the lifetime fix — `EmDocument.h` / `EmDocument.cpp`.**

6a. `EmDocument.h`: change `ScheduleDialog`'s return type from `void` to
`EmAction*`.

6b. `EmDocument.cpp` `ScheduleDialog` (~705-713):

```cpp
EmAction* EmDocument::ScheduleDialog (EmDlgThreadFn fn,
									  const void* parms,
									  EmDlgItemID& result)
{
	EmAction*	action = new EmActionDialog (fn, parms, result);
	this->PostAction (action);
	return action;
}
```

6c. `EmDocument.cpp` `EmActionDialog::Do` (132-142):

```cpp
void EmActionDialog::Do (void)
{
	// Show any error messages from the CPU thread.

#if HAS_OMNI_THREAD
	EmAssert (gSession);

	// The parameters and the result reference point into the CPU thread's
	// BlockOnDialog stack frame.  BeginDialogAction returns false if that
	// frame is gone (the request was cancelled by fStop/fReset) -- in that
	// case touching fDlgParms or fDlgResult is a use-after-free.

	if (!gSession || !gSession->BeginDialogAction (this))
		return;

	fDlgResult = EmDlg::RunDialog (fDlgFn, fDlgParms);

	gSession->EndDialogAction ();
#else
	fDlgResult = EmDlg::RunDialog (fDlgFn, fDlgParms);
#endif
}
```

- [ ] **Step 7: Rebuild.**

Run: `cmake --build build -j16`
Expected: exit 0. (`grep -rn "UnblockDialog" src/` must return nothing.)

- [ ] **Step 8: Run the extended repro — confirm PASS, three times.**

Run: `cd tests/phase1 && for i in 1 2 3; do python3 repro_dialog_subsystem.py || break; done`
Expected: `PASS - deferred-error dialogs survive reset in both interleavings` × 3.
(Three runs because Part 3 is racing a 100 ms window; the loop inside the repro
plus three whole-script runs gives 12+ samples of the queued interleaving.)

- [ ] **Step 9: No-regression check on the already-shipped Phase 1 fixes.**

Run: `cd tests/phase1 && python3 repro_1_8_argval.py && python3 repro_1_3_proxy.py`
Expected: `PASS 1.8` and the 1.3 PASS lines.

- [ ] **Step 10: ASAN verification (this is a UAF fix — prove it to ASAN).**

```bash
cmake --build build-asan -j16   # configure first if the dir is empty:
# cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
#   -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1" \
#   -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cd tests/phase1 && python3 -c "
import _harness" # then run the repro against the ASAN binary:
python3 repro_dialog_subsystem.py    # with _harness emulator(build='build-asan')
```

Concretely: temporarily run the repro with `emulator(PORT, build="build-asan")`
(the harness already takes a `build=` kwarg) or parameterize via env var —
implementer's choice, but the repro as committed must still target `build/`.
Expected: PASS with **zero ASAN reports** in the emulator's output.

- [ ] **Step 11: Update docs (R5 — same commit).**

- `docs/STATUS.md` landmine #9: rewrite as FIXED — verified wording only:
  the dialog shows within one idle tick (the "never shows" claim was
  disproven 2026-06-10); the crash was the BlockOnDialog action-lifetime
  UAF, fixed by the queued/running/done/cancelled handshake; repro named.
- `docs/architecture.md`: in the threading section, document the lifetime
  contract: *a scheduled `EmActionDialog` references the CPU thread's
  `BlockOnDialog` stack frame; `BlockOnDialog` never returns until the action
  is cancelled-while-queued or has completed; UI-side entry/exit is
  `BeginDialogAction`/`EndDialogAction`.* Add "do not post actions that
  reference stack data without such a handshake" to the Do-Not-Do list.
- `docs/recontrol-protocol.md`: no protocol change (`reset` response text
  unchanged) — only touch it if the implementer changed any response.
- **Progress trackers** (same commit): in `docs/recovery-plan-2026-06.md`,
  check the 1.0d box and update the CURRENT POSITION banner at the top to
  "NEXT TASK: 1.1"; in `docs/STATUS.md`, update the "Recovery progress"
  section the same way.

- [ ] **Step 12: Commit.**

```bash
git add tests/phase1/repro_dialog_subsystem.py \
        src/core/EmSession.h src/core/EmSession.cpp \
        src/core/EmDocument.h src/core/EmDocument.cpp \
        docs/STATUS.md docs/architecture.md
git commit -m "fix(1.0d): BlockOnDialog action-lifetime handshake (reset-during-dialog UAF)

A scheduled EmActionDialog references the CPU thread's BlockOnDialog stack
frame (dialog params + result). fStop/fReset broke the wait and let the
frame die with the action still queued (UAF read -> SIGSEGV) or running
(UAF write through fDlgResult -> stack corruption). BlockOnDialog now
flags a still-queued action cancelled or waits for a running one to
finish; EmActionDialog::Do brackets with BeginDialogAction (skips if
cancelled) / EndDialogAction. UnblockDialog deleted (replaced, R2).

Also corrects the 2026-06-10 finding: the 'dialog never shows' hang did
not reproduce -- dialogs show one idle tick (~100ms) after blocked_on_ui;
the repro now guards that latency too.

Repro: tests/phase1/repro_dialog_subsystem.py (both interleavings, ASAN-clean)"
```

---

## Gate for this task

`repro_dialog_subsystem.py` passes 3× consecutively against `build/` AND once
against `build-asan/` with zero sanitizer reports. Then tasks 1.1/1.2 are
unblocked: the 1.1 repro's `_force_blocked` helper is `spy set 0x134` (blocked
within ~1 s), dismissal is `dialog respond continue`, cleanup is `spy clear`
(note: after `respond continue` the spy re-fires within ~10 ms, so the repro
should either loop respond-until-`spy clear`-lands or use post-1.0d `reset` as
the final cleanup — finalize against the live build per the existing 1.1
execution-note discipline).

## After this task — getting back on course

Continue with **Task 1.1** in
`docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` (the
already-written, deliberately-reverted fix diff is in that doc's Task 1.1 —
re-apply it only through its repro-first steps). Then 1.2 → 1.4 → 1.7 →
1.5/1.6 (TSAN, with their STOP rules) → GATE 1, per that plan and the ranked
list in `docs/recovery-plan-2026-06.md`. One phase-task per sitting (R6).

## Self-review (writing-plans)

- **Spec coverage:** both verified crash interleavings have repro parts and are
  addressed by one mechanism; the disproven hang gets a regression guard;
  docs corrected in the same commit. ✓
- **Placeholders:** full code for every step; the only deferred decision
  (ASAN-run plumbing in Step 10, 1.1 cleanup recipe) is explicitly delegated
  with options, matching the plan-set's execution-note convention. ✓
- **Type/name consistency:** `fDialogActionState`/`fDialogAction`/
  `BeginDialogAction`/`EndDialogAction`/`kDlgAction*` used consistently across
  Steps 4-6; `ScheduleDialog` return-type change matches both 6a and 6b and the
  call site in 5b. ✓
- **Deadlock review:** the only new wait is the CPU thread waiting on
  `fSharedCondition` (releases `fSharedLock`) for `kDlgActionRunning →
  kDlgActionDone`; the UI thread's `EndDialogAction` acquires `fSharedLock`
  only after `msgBox.exec()` has returned, and nothing on the UI thread ever
  blocks on the CPU thread — no cycle. `BeginDialogAction` vs the CPU's
  early-exit both run under `fSharedLock` — the queued/running decision is
  atomic. Stale-action identity is by pointer compare only (never
  dereferenced), and a stale action cannot be confused with a newer one
  because both are live allocations while compared. ✓
