# Landmine Hardening — Sequencing Design + Sub-project 1 (#6)

- **Date:** 2026-06-14
- **Status:** APPROVED (sequencing + #6 approach). Sub-projects 2–4 are sketches; each gets its own spec → plan → implementation cycle.
- **Branch (this sitting):** `landmine-6-paintscreen-race`
- **Baseline:** 0.9.1 @ `94058a3` (master); fresh full-suite run 2026-06-14 = 22/23 deterministic + HotSync smoke + live MCP all green.

## Context

After Phases 1–5, three landmines remain open in `docs/STATUS.md`:

- **#5** — Two threads can run the 68K core concurrently (rare). A second
  `SuspendThread(kStopOnSysCall)` succeeds trivially while the first is mid-
  `ExecuteSubroutine`; the `kStopOnSysCall` result switch (`EmSession.cpp:998-1005`)
  has no ownership/nesting check, so a GUI menu action concurrent with a worker-
  thread ROM call can corrupt UAE's global `regs`. Suspected source of "random"
  historical corruption.
- **#6** — `PaintScreen` reads LCD state without stopping the CPU. The GUI window
  repaint (main thread T0) calls `EmScreen::GetBits` at `EmWindow.cpp:545` with no
  `EmSessionStopper`; inside, `CEnableFullAccess` does torn writes to the global
  `gMemAccessFlags` (`EmMemory.cpp:629-658`) while the CPU thread (T1) reads it on
  every memory access → data race. A Qt6 port regression — upstream POSE stopped
  the CPU here.
- **#11** — Teardown/lifecycle crashes, observed-once each, "recorded not chased":
  (a) quit-path SIGSEGV (prefs touched during teardown while the CPU thread still
  runs); (b) `load_during_queue` SEGV under TSAN (deferred err-watchpoint dialog
  scheduling hit a null QMutex while `load` swapped the document; 1.0d-adjacent).

None are documented GATE 5 / v1.0 blockers.

### Decisions (user, 2026-06-14)

1. **Order by reproducibility-first** (R1): the bug we can make a test fail on
   deterministically goes first.
2. **Harden before shipping 1.0**: fix the reproducible landmines now, then do
   packaging + GATE 5.

## Sequence (decomposition into four sittings)

Each landmine is its own sitting with a gate before stopping (R6).

| Order | Sub-project | Rationale | Repro difficulty | Expected fix size |
|-------|-------------|-----------|------------------|-------------------|
| 1 | **#6** PaintScreen LCD-read race | Most reproducible; TSAN flags it; small intent-matching fix; builds the real-display TSAN harness #5 reuses | Medium (deterministic under TSAN) | Small |
| 2 | **#5** two-thread 68K race | Same family as #6, reuses its harness; biggest correctness payoff | Hard (rare race) | Medium |
| 3 | **#11a** quit-path SIGSEGV | Phase 5 already routes the normal-quit prefs save to *after* CPU-worker shutdown (`main.cpp`); likely verify-and-close | Verify only | None expected |
| 4 | **#11b** load-during-queue | Hardest; different subsystem (document/dialog lifecycle); may stay open with an explicit decision | Hard (observed once) | Unknown |

After sub-project 4: packaging (deb/AppImage/exe) + GATE 5 → ship 1.0.

### Binding process rules (recovery-plan R1–R6)

R1 reproduce-first (failing repro committed before any fix); R2 replace-don't-stack;
R3 assert on emulator effects, not `OK`; R4 clean tree per sitting; R5 docs land in
the same commit as the behavior change; R6 one sitting per landmine, gate before stop.

### Cross-cutting harness note

#5/#6 are concurrency races. The gate is a **real-display TSAN run** (`build-tsan/`
on DISPLAY :1), because the offscreen Qt platform may not exercise the GUI paint path
and the project already records that TSAN verification "requires a real display"
(STATUS landmine #1.7). The pass condition is "TSAN race report present → absent,"
not "no crash." The known pre-existing TSAN "baseline families" include the #5/#6
sites; after #6 lands, re-baseline so #5's remaining race is isolated.

## Sub-project 1 — Landmine #6 (detailed)

### Bug (confirmed in code)

- `EmWindow.cpp:545` — `bufferDirty = EmScreen::GetBits(info)` called from the
  main-thread paint with no `EmSessionStopper`.
- `EmScreen::GetBits` constructs a temporary `CEnableFullAccess` which, in
  `EmMemory.cpp:630-635` (ctor) sets `gMemAccessFlags = kZeroMemAccessFlags` and in
  `:651` (dtor) restores it — non-atomic writes to a global the CPU thread reads
  on every guest memory access. Data race.
- `GetBits` is called from **two** sites in `EmWindow.cpp`; only the paint one is
  unguarded:
  - `EmWindow::GetLCDContents` (`:462`, used for screenshots) — **already guarded**
    by `EmSessionStopper stopper (gSession, kStopNow)` at `:457`. **Not** the bug,
    and the fix template for the other site.
  - `EmWindow::PaintScreen` (`:545`, Qt GUI window repaint ~10 Hz, main thread T0) —
    **unguarded** (no stopper anywhere in the function). **This is the bug.**
  - (The ReControl `screenshot`/`screen-hash` commands go through the guarded
    `GetLCDContents`/worker-cycle path, so they are safe.)

> **CRITICAL FINDING (2026-06-14, code-audited — changes the fix direction).**
> `PaintScreen` does not merely *lack* a stopper; the stop was **deliberately
> removed**. `EmWindow.cpp:485-489` comments: *"Read hardware state WITHOUT
> suspending the CPU. A torn read is acceptable for display — we just want pixels.
> This avoids the SuspendThread deadlock that occurs when the UI thread tries to
> pause a CPU in nested subroutine execution."* So approach 1 (add a stopper) was
> already tried and reverted because it deadlocks the main thread against a nested
> CPU. The authors accepted torn *pixel* reads. The actual landmine is narrower and
> worse: `CEnableFullAccess` mutates the **process-global** `gMemAccessFlags`
> (`EmMemory.h:332`, read on the CPU's hot memory path), so the paint doesn't just
> tear pixels — it transiently corrupts the CPU thread's memory-access-checking
> state. The fix must target that global, ideally without reintroducing the stop.

### Reproduce (R1 — first commit, must FAIL before any fix)

On `build-tsan` (already configured: `-fsanitize=thread -O1 -g`), real display
(DISPLAY :1, since the offscreen platform may not paint): drive continuous guest CPU
activity (e.g. `gremlin new`) while the GUI window repaints, and assert TSAN reports a
data race whose stack names `gMemAccessFlags` / `CEnableFullAccess` / `PaintScreen`.
Test fails (race present) pre-fix; passes (that specific signature absent) post-fix.
Assert on the **specific** signature, not "zero reports," since other baseline families
(#5) persist until later.

**Harness gap to close first:** there is no committed TSAN test harness or
suppressions file (only `ubsan.supp`). The repro must establish: building/refreshing
`build-tsan`, launching it under a real display via the existing
`tests/lib/harness.py` (`build="build-tsan"`, `env_extra` overriding
`QT_QPA_PLATFORM=xcb` + `DISPLAY`), setting `TSAN_OPTIONS` (e.g. `halt_on_error=0`,
`log_path`/stderr capture via `capture_log`), and grepping the captured report for the
signature. This harness is itself a deliverable reused by #5.

### Fix — approaches (decide AFTER the repro exists, per R1)

The repro must come first; it is also the bench we test candidate fixes against.
Given the CRITICAL FINDING, the ranking is now:

1. **Make the access-flag swap thread-safe without stopping the CPU** *(now primary).*
   Stop `CEnableFullAccess` from mutating the process-global `gMemAccessFlags` while
   the CPU reads it. Candidate: make the "full access" override **thread-local** (the
   paint thread temporarily zeroes *its own* view; the CPU thread's view is untouched)
   — respects the authors' deliberate "don't stop the CPU for paint" decision.
   - **Required first step:** survey every reader/writer of `gMemAccessFlags` across
     threads (it is consulted on the memory hot path), to confirm a thread-local split
     is correct and to bound the perf impact. It is a non-scalar struct, so a plain
     `std::atomic` does not apply.
2. **Re-introduce a *bounded, deadlock-safe* CPU stop** *(fallback).* Approach 1 from
   the original draft. The `EmWindow.cpp:485` comment shows a naive stopper deadlocks
   on a nested CPU — BUT landmine #10 (nested-ROM deferral via `CheckForBreak`
   masking) and task 1.2 (bounded 5000 ms stops) both landed *after* that comment.
   So a bounded `kStopOnCycle` *might* now be safe where `kStopNow` was not. Only
   pursue if approach 1 proves infeasible, and prove non-deadlock against the repro
   under the nested-call case before trusting it.
3. **Double-buffer the LCD.** CPU thread publishes scanlines to a buffer the paint
   reads lock-free. Biggest change; last resort.

### Testing / gate

- New TSAN repro flips fail → pass.
- Full deterministic sweep stays green (22/23; the 23rd is the documented idle-host
  connection-guard flake, not a regression).
- Idle-CPU cost unchanged vs baseline (~80.5%).
- One commit, docs in it (R5): STATUS landmine #6 struck through with the fix +
  evidence; architecture.md updated if the main-thread stop semantics change;
  recovery-plan banner noted.

### Files likely touched

- Repro: `tests/phase6/` (new) or `tests/phase2/` — a TSAN race test + harness glue.
- Fix (approach 1): `src/core/EmWindow.cpp` (paint path); possibly a small helper in
  the session/stopper area. Approach 2 would instead touch `src/core/Hardware/EmMemory.cpp`.
- Docs: `docs/STATUS.md`, `docs/architecture.md` (if semantics change),
  `docs/recovery-plan-2026-06.md` (banner).

## Sub-project 2 — Landmine #5 (detailed)

### Bug (confirmed in code, 2026-06-14)

`SuspendThread(EmStopMethod, timeoutMs)` (`EmSession.cpp:774`) stops the CPU thread.
For `kStopOnSysCall`, the first switch (`:808`) only sets `desiredBreakOnSysCall`;
the actual wait happens in the `while (fState == kRunning)` loop (`:860`). **If the
CPU is already `kSuspended`** (e.g. another caller already stopped it on a syscall),
that loop is skipped entirely and control falls to the result switch:

```cpp
case kStopOnSysCall:                                   // EmSession.cpp:998
    result = (fState == kSuspended) && fSuspendState.fCounters.fSuspendBySysCall;
    if (result)
        fSuspendState.fCounters.fSuspendByUIThread++;  // claims ownership it didn't earn
```

There is **no ownership/nesting check**: a second caller sees the *first* caller's
`fSuspendBySysCall` already set and "succeeds" without having driven the CPU to stop.
The window is real because `ExecuteSubroutine` (the host-initiated ROM-call path)
**releases `fSharedLock` during `CallCPU()`** (`:1343-1346`). So a GUI/main-thread
action overlapping a worker-thread ROM call can leave two code paths both believing
they own a stopped-on-syscall CPU and both manipulate UAE's global `regs` → corruption.
The nesting comment at `:1313-1316` is the landmine-#10 *deferral* (masking
`fSuspendByUIThread` while nested); it does **not** guard this two-owner case.

### Reproduce (R1 — must FAIL first; real-display TSAN)

Harder than #6: needs **two concurrent CPU-driving paths**. Convention exit 0 =
fixed, exit 1 = race present.

- **Investigation first (the crux):** survey callers of `ExecuteSubroutine` /
  `CallCPU` / `ATrap::DoCall` and classify them by thread (worker ReControl handlers
  vs main-thread Qt idle/menu). Find a pair that can overlap (ReControl is
  single-connection, so the second driver is almost certainly a main-thread path).
- **Repro:** under `build-tsan`, real display, hammer the worker ROM-call path
  (e.g. repeated `apps`/`launch`, which call `CollectCurrentAppInfo` family) while
  the main thread independently drives ROM/idle work, and assert TSAN reports a data
  race on the UAE register globals (`regs`) with `ExecuteSubroutine`/`CallCPU` frames
  from **two different threads**. Match the specific signature (not the #6 family).

### Fix — approaches (decide after repro)

1. **Ownership/nesting guard at the result switch** *(primary candidate).* A second
   `kStopOnSysCall` must not claim success against a CPU it did not stop. Track a
   single syscall-stop owner (or refuse when `fNestLevel > 0` / when the stop was not
   this caller's), returning `false`/`ERR busy` instead of a phantom success.
2. **Serialize ROM-call drivers** with a dedicated mutex/owner token around
   `ExecuteSubroutine`, so only one host-initiated ROM call is in flight at a time.

**Risks:** must not regress #10 (nested-ROM deferral via `CheckForBreak`), #2/#4
(suspend-counter balance and bounded timeouts), or #1.0d dialog lifetime. Reuses #6's
real-display TSAN harness. **Plan:** `docs/superpowers/plans/2026-06-14-landmine5-reproduce.md`.

## Sub-projects 3–4 (sketches — not yet designed)

- **#11a:** verify Phase 5's reorder actually closes the normal-quit window (hammer
  quit-while-CPU-busy under ASAN); strike through or document residual.
- **#11b:** reproduce the document-swap-vs-deferred-dialog race under TSAN; extend the
  1.0d dialog-lifetime state machine to cover document swap, or make an explicit
  "defer past 1.0" decision with rationale.

## Non-goals / out of scope

- Packaging and GATE 5 (after the hardening sittings).
- Any refactor beyond what each fix needs (no opportunistic rewrites).
- Fixing #5/#11b in this sitting — #6 only.
