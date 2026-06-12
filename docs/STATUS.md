# POSE64 — Project Status

**Date:** 2026-06-09 (full code + docs audit; previous activity 2026-04-03);
Phase 1 progress updates 2026-06-10; **Phase 2 COMPLETE — GATE 2 PASSED 2026-06-11**;
**Phase 3 COMPLETE — GATE 3 PASSED 2026-06-12** (first run same day FAILED on
two gaps, both root-fixed + re-run PASSED; findings + resolution:
`docs/superpowers/plans/2026-06-12-gate3-fail-findings.md`).
**Phase 4 COMPLETE — GATE 4 PASSED 2026-06-12: HotSync verified end-to-end
against pilot-link** (procedure: `docs/hotsync.md`; root-cause record:
`docs/superpowers/plans/2026-06-12-phase4-findings.md`). **Next: Phase 5
(declutter and ship 0.9.1).**
**Read this first.** This file is the only document guaranteed to describe the
project as it IS. Architecture details: `docs/architecture.md`. Protocol:
`docs/recontrol-protocol.md`. Everything in `docs/history/` is a dated
historical record — do not treat it as current.

## What POSE64 is

A native 64-bit Qt6 port of Palm OS Emulator (POSE) 3.5 for reverse
engineering old Palm applications, driveable by AI agents through a TCP
control protocol (ReControl, port 6416) and an MCP proxy (`pose64-mcp-proxy`).
Stretch goal: timing accurate enough to HotSync against pilot-link on the
host.

## What verifiably works (code-audited 2026-06-09)

- **Core emulation**: PalmOS 2.0–4.1 ROMs, DragonBall 328/EZ/VZ, accurate
  cycle-fed timers (default on), 1x wall-clock throttle (default), per-device
  calibration (m500 only), sleep-until-interrupt idle.
- **ReControl**: 36 commands via a table-driven dispatcher with explicit
  threading categories (`ReControl.cpp:101`). The dispatch-category
  assignments were audited command-by-command and are correct. (`break`
  deliberately moved WorkerCycle → Adaptive 2026-06-12, GATE 3 Gap 1: it
  runs directly while `blocked_on_ui` — the frozen CPU thread cannot touch
  the breakpoint table — so breakpoints can be cleared from the dialog
  before resuming; `load` refuses while `blocked_on_ui` instead of
  deadlocking, same date.)
- **MCP proxy**: **37** `palm_*` tools served from a single source-of-truth
  table (tools/list, dispatch, and reconnect/idempotency policy all derive
  from it — Phase 3a). Central argument validation: a missing/invalid
  argument is `ERR usage`, never a silent default. The full debug surface
  (backtrace/break/watch/spy/log/gremlin/check/errorhandling/profile/speed)
  is MCP-exposed; drift between proxy and SKILL.md is test-gated
  (`tests/phase3/test_mcp_surface.py`).
- **Debug surface that works today**: `peek/poke/regs/backtrace`,
  `screenshot` (+scale/grid/annotate/crosshair overlays), `screen-hash`,
  `ui`, `watch`/`spy` (raise dialogs), `log` (20 categories), `gremlin`,
  `errorhandling`, `profile` (full Metrowerks .mwp output), session
  save/load, `dialog` query/respond with register dump.
- **Serial**: a full PTY transport (`pty:HotSync` in Preferences) is built in
  and prints its `/dev/pts/N` path at startup; cradle button is scriptable.

## Landmines (verified, with mechanism)

1. ~~**`break` is passive without an external debugger.**~~ **FIXED (Phase 3b,
   2026-06-11, task 3.2).** Previously a breakpoint hit only suspended the CPU
   if a Palm-Debugger SLP client was attached; with none, `EnterDebugger`'s
   no-debugger fallback just logged "Failed to enter debug mode" and resumed —
   the hit was silently ignored. **Mechanism:** that fallback now schedules an
   `EmDeferredErrBreakpoint` (modeled byte-for-byte on `EmDeferredErrWatchpoint`,
   holding only `(int index, emuptr pc)` by value — no CPU-stack pointers) via
   `gSession->ScheduleDeferredError(...)`, the same CPU-thread-safe path
   `DoCheckWatchpoint` uses. `Errors::ReportErrBreakpoint` renders the
   `kStr_ErrBreakpoint` Continue/Debug/Reset dialog (slot index + hit address +
   register dump), so the CPU blocks on `blocked_on_ui` and resumes via
   `dialog respond continue`. With `--slp-debugger`, the external debugger still
   takes the hit instead. **Evidence:** `tests/phase3/test_break_real.py` 3× PASS
   (each run 3 rounds: hit → blocked_on_ui → dialog inspected → resume); TSAN
   spot-check clean (0 reports implicate the new break path; only the documented
   #5/#6 baseline families); `repro_dialog_subsystem.py` (the 1.0d dialog
   lifecycle this rides) still PASS.
2. ~~**Suspend-counter leak ⇒ unrecoverable freeze.**~~ **FIXED (task 1.1,
   2026-06-10).** `SuspendThread(kStopNow/kStopOnCycle)` incremented
   `fSuspendByUIThread` unconditionally in the first switch, then returned
   false without decrementing when the CPU was `kBlockedOnUI` (wait loop
   skipped, result switch saw `fState != kSuspended`). The caller
   (`EmSessionStopper`) saw `Stopped()==false` and did NOT call `ResumeThread`,
   so the counter leaked. After the next `BlockOnDialog` returned,
   `CheckForBreak` saw `fAllCounters != 0` and parked the CPU permanently.
   **Fix:** `SuspendThread` now decrements `fSuspendByUIThread` on the failure
   path for `kStopNow`/`kStopOnCycle` (`EmSession.cpp` after the result
   switch), making failure side-effect-free. `ForceReset` now clears the
   counter as a last-resort safety net (the "Do NOT clear" guard was a
   workaround for this leak, not a correctness requirement).
   Repro (3× PASS): `tests/phase1/repro_1_1_suspend_leak.py`.
3. ~~**Input is fire-and-forget.**~~ **FIXED (Phase 2, 2026-06-10 — commit
   `e177361` honest contract (Tasks 6+7) + the wake-hook commit (Task 8,
   this commit).** `tap`/`tap-id`/`pen`/`key`/`type` are now delivery-honest:
   each posts the event and blocks ≤2 s on a per-queue delivery counter that
   PuppetString increments once the event reaches the Palm OS event queue
   (`Notify{Key,Pen}EventDelivered`). `OK delivered` means the guest has it; a
   refused post returns `ERR busy:…` / `ERR duplicate:…`, an undelivered one
   `ERR pending`. The asleep-guest gap is closed by the **one** input-delivery
   wake mechanism — a STOP-exit `EvtWakeup` hook at the bottom of
   `ExecuteStoppedLoop` (approach B): on STOP-exit with input pending (awake,
   UI initialized) it signals the event group so SysEvGroupWait returns,
   worst-case one timer tick. The old `PrvWakeUpCPU` and the 2026-03-13
   poll-always patch (approach A) were **deleted** (R2/2.3). `button` keeps the
   queued contract and reports `ERR busy` when refused. *(Task 1.8
   arg-validation, commit `5f5c443`, still stands: `tap banana` → `ERR usage`.)*
   Verified on master: idle delivery 8/8 100% (p50 234 ms),
   `tests/phase2/test_honest_ack.py` PASS, phase-1 repros 7/7, app-switch repro
   3/3 clean (2,400 switches). **GATE 2 TSAN race-check RUN & PASSING 2026-06-11**
   (delivery machinery + hook TSAN-clean after annotating `omni_condition` waits
   so TSAN can see through `QWaitCondition::wait`). **GATE 2 PASSED 2026-06-11**
   (Task 9 all steps): 200-tap matrix **200/200 (100%)** at 1x+Max (0/400 fails),
   idle-CPU 1x median **80.65%** (baseline 80.50%, hook cost ≈ 0), grep gate
   clean (one input-delivery wake), tagged `phase-2-complete`. Detail: handoff §11–§12
   + `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md`.
   *(NB: the "one wake mechanism" grep gate means one* input-delivery *wake;
   `EvtWakeup` legitimately appears in Gremlins, app-switch/`launch`,
   post-reset bootstrap, and file import on their own paths.)*
4. ~~**Untimed stops can wedge the whole control plane.**~~ **FIXED (task 1.2,
   2026-06-10).** `kStopNow`/`kStopOnCycle` stoppers previously had no timeout
   (`EmSession.cpp` — `useTimeout` was gated on `how == kStopOnSysCall`). A CPU
   nested in a ROM call could park the worker thread forever via a livelock in
   `SuspendThread`'s wait loop (both sides broadcasting on `fSharedCondition`
   without `fState` leaving `kRunning`). **Fix:** `useTimeout = (timeoutMs > 0)`
   (gate removed); `kCmdWorkerCycle` and `kCmdAdaptive` dispatch now pass 5000ms
   and return `ERR timeout: ...` with a recovery hint instead of the generic
   `ERR transient: could not stop session`. The timeout return path also balances
   the `fSuspendByUIThread` increment (consistent with the 1.1 fix).
   Repro (3× PASS): `tests/phase1/repro_1_2_stop_timeout.py`.
   *The proxy half is also fixed (task 1.3, commit `08d8690`):* the MCP proxy
   sets `SO_RCVTIMEO` (a wedged server yields `ERR timeout` instead of hanging)
   and never double-executes non-idempotent commands. Verified:
   `tests/phase1/repro_1_3_proxy.py`.
5. **Two threads can run the 68K core concurrently (rare).** A second caller
   of `SuspendThread(kStopOnSysCall)` succeeds trivially while the first is
   mid-`ExecuteSubroutine` (`EmSession.cpp:979-986`); a GUI menu action
   concurrent with a worker-thread ROM call can corrupt UAE's global `regs`.
   Likely source of "random" historical corruption.
6. **`PaintScreen` reads LCD state without stopping the CPU** and
   `EmScreen::GetBits` swaps the global `gMemAccessFlags` while the CPU runs
   (`EmWindow.cpp:549-607`, `EmMemory.cpp:630-657`) — a port regression (the
   original stopped the CPU here).
7. ~~**`check set` re-arms a known freeze.**~~ **ROOT-FIXED (landmine-7 root
   fix, 2026-06-12, branch `landmine-7-root-fix` — deferral overridden by
   user decision).** Any DRAM-region check flag previously re-armed unbounded
   per-DRAM-access work. **Mechanism of the freeze (root-caused 2026-06-12):**
   `META_CHECK` fires per DRAM access from a RAM PC; `GetWhatHappened` does a
   full heap walk (`GWH_ExamineHeap`) and `AllowForBugs` runs several `In*`
   function-range predicates per access — and `EmFunctionRange::InRange`
   never caches a non-match and deliberately `Reset()`s ranges found in RAM
   (`EmPalmFunction.cpp`), so each predicate re-runs `FindFunctionName`, a
   2-byte-step scan across the PC's whole containing chunk. Nothing remembered
   a verdict, so one hot site paid full price per access (measured: ONE PC,
   3.2M analyses in ~2 min, 100% non-OK verdicts — most dropped *unreported*
   because their class's Report pref wasn't even armed). Reported violations
   additionally grew RSS per report (`EmEventPlayback::RecordErrorEvent` +
   `LogDump`).
   - **Pre-fix reproduction (2026-06-12, C2+instrumentation-strip binary,
     ScreenAccess + `gremlin new 42 2000000`, 10-min acceptance):**
     **ACCEPTANCE FAIL** — episodic 99%+ CPU bursts with RSS jumps, then a
     sustained pin: first-2-min CPU median 37.5% → last-2-min **99.8%**; RSS
     growth **+98.8 MB**. Matches the Phase 3c "freeze relocates/returns"
     evidence (instant-pin in the 3c C1 run; burst timing varies with the
     gremlin's app mix).
   - **The fix (two layers):** (1) Phase 3c C2 re-applied — negative caching
     in `PrvSearchForCodeChunk` + insert dedup in `PrvAddTaggedChunk` (cures
     the first-order database×resource re-walk). (2) **Per-site verdict
     cache** at the `EmBankDRAM::ProbableCause` boundary keyed
     `(PC, size, meta-bit signature, r/w)`: first occurrence per site runs
     the full existing analysis + report path byte-for-byte (including the
     `Report*Access` pref gate); repeats are counted and suppressed.
     Invalidation: atomic generation bumped on any `Report*Access` pref
     change / `Reset` / `Load`, applied lazily on the CPU thread; precise
     per-chunk erase in `MetaMemory::ChunkUnlocked` (entries are
     chunk-anchored; PCs outside any heap chunk are never cached).
     Forgiveness paths that depend on dynamic context (stack walks, live UI
     objects, transient patch state, address-conditioned checks) call
     `MarkVerdictUncacheable` and are never cached — violation-detection
     correctness is preserved by construction. **Reporting-semantics change
     (documented in recontrol-protocol.md + palm_check):** one report per
     site per arming; `check clearall` + `check set` re-arms fresh reports.
   - **Evidence:** `tests/phase3/test_check_suppression.py` 3× PASS (CPU flat
     at baseline with the hot class armed; one `blocked_on_ui` dialog per
     arming; `dialog respond continue` resumes with no re-block; re-arm
     re-reports exactly once). Post-fix acceptance: see Phase 3 bullet
     (ScreenAccess + all-six-flags 10-min runs, spec §C2 bars). Per-access
     fprintf instrumentation (`META_ERROR`/`ALLOWBUGS`/`GETRANGE`/`MEMMGR`
     traces, committed in `870b7ab`) was stripped en route — it polluted
     every prior measurement of this landmine.
8. ~~**SLP debugger sockets listen by default** (6414/2000) and connecting
   triggers an untimed main-thread `kStopOnSysCall` stop
   (`Debug::EventCallback`) — a UI hang waiting to happen.~~ **FIXED (Phase
   3b, 2026-06-11).** Two-part defuse: (1) `Debug::CreateListeningSockets`
   now early-returns unless the default-false `SLPDebugger` preference is set
   or the `--slp-debugger` CLI flag forced sockets on this run
   (`gForceDebuggerSockets` via `Debug::ForceSocketsThisRun`) — by default
   nothing listens on 6414/2000, so the connect path is never armed; (2) both
   `Debug::EventCallback` stoppers (the `kConnected` FtrSet and `kDisconnected`
   FtrUnregister) are now bounded at 5000 ms (task-1.2 pattern) with a graceful
   `else` skip if the CPU does not stop — so even when opted in, a connect can
   no longer wedge the UI thread indefinitely. Repro (3× `ALL PASS`):
   `tests/phase3/repro_slp_trap.py` — part 1 asserts default-off refusal,
   part 2 drives a >5 s connect/disconnect probe proving the control plane
   stays responsive.
9. ~~**`reset` during a deferred-error dialog crashes the process.**~~ **FIXED
   (task 1.0d, 2026-06-10).** A scheduled `EmActionDialog` referenced the CPU
   thread's stack-local dialog data (`RunDialogParameters` + `EditCommonDialogData`).
   `reset` broke the `BlockOnDialog` wait early, letting the stack die while the
   action was still queued or running. Two interleavings: reset while *queued*
   (≤100 ms idle-tick window) → UAF read of `fMessage` → SIGSEGV; reset while
   *showing* → `Do` writes the dangling `fDlgResult` → stack corruption.
   **Fix:** `BlockOnDialog` now tracks dialog-action lifetime via a
   queued/running/done/cancelled state machine (`fDialogActionState`, guarded by
   `fSharedLock`). A still-queued action is flagged cancelled (it then no-ops in
   `BeginDialogAction`); a running action is waited out before the stack unwinds.
   `UnblockDialog` replaced (R2) by `BeginDialogAction`/`EndDialogAction`.
   Repro (3× PASS, ASAN-clean): `tests/phase1/repro_dialog_subsystem.py`.
   *Dialog-show latency confirmed:* the dialog shows one idle tick (~100 ms)
   after `blocked_on_ui`; the earlier "never shows" report was a mis-observation.
10. **App-switch churn killed the guest — FIXED 2026-06-10 (root-caused).**
   Symptom: sustained app switching intermittently raised
   `SysFatalAlert "MemoryMgr.c"` line 4365 `NULL handle` / 4384 `Free
   handle` / 4415 `Invalid handle` → `blocked_on_ui`. **Root cause (not
   app-switch-specific):** the Qt6 port made `EmSession::ExecuteSubroutine`
   BREAK OUT of a nested host-initiated ROM call when a
   `kStopNow`/`kStopOnCycle` suspend (`screen-hash`, `ui`, `peek`, paint —
   anything raising `fSuspendByUIThread`) arrived mid-call; `ATrap::DoCall`
   /`EmSubroutine` never check the exit reason, so the stub's caller read
   garbage out of D0/A0 (upstream POSE 3.5 instead DEFERS the suspend until
   the subroutine completes — verified against vendored source). App
   switches make ~10 host ROM calls each (`CollectCurrentAppInfo` family),
   so churn + polling maximized the collision odds. Instrumented proof:
   every crash had a nested-abort logged 0–2 lines before the fatal alert;
   clean runs had none (or one benign). **Fix:** deferral restored —
   `CheckForBreak` masks `fSuspendByUIThread` while `IsNested()` (counter
   stays live for timeout-undo/`ResumeThread` accounting),
   `ExecuteSubroutine` re-arms `CheckAfterCycle` on exit. **Verified:** hot
   repro (`--hammer 3`, crashed 3/6 runs pre-fix at switches 33–438) →
   **0/6 crashes post-fix (4,800 switches)**; phase-1 repros 7/7 PASS;
   `test_speed_cmd` PASS; TSAN stress 13/13 PASS with report families
   matching the master baseline (zero reports implicate the suspend
   machinery). Repro: `tests/phase2/repro_appswitch_memmgr.py` (now exits
   0 = no crash, phase-1 convention; `--hammer N` = hot mode). Un-caps
   rapid-mode GATE-2 runs. Detail: handoff §12.4 (RESOLVED note).
11. **Teardown/lifecycle crashes, observed-once each (2026-06-10, recorded
   not chased).** (a) Quit-path SIGSEGV: CPU thread in
   `EmRegsVZ::CycleSlowly → EmUARTDragonball::GetTransport →
   EmulatorPreferences::GetTransportForDevice` while the main thread saved
   XML prefs in libexpat — prefs used during teardown while the CPU thread
   still runs (core: coredumpctl PID 1369462). (b) `load_during_queue` SEGV
   under TSAN (1 of 3 B-branch runs, 0 of 3 master runs): deferred
   err-watchpoint dialog scheduling (`EmDocument::ScheduleDialog →
   EmActionHandler::PostAction`) hit a null QMutex while `load` swapped the
   document — dialog/load lifecycle family, 1.0d-adjacent. Phase-2 hook
   absent from both stacks. Detail: handoff §12.5.

## Recovery progress (read `docs/recovery-plan-2026-06.md` for the roadmap)

- **Phase 0 — complete 2026-06-10** (GATE 0 passed; details below).
- **Phase 1 — GATE 1 PASSED (2026-06-10).** Done & verified: **1.8** (`5f5c443`),
  **1.3** (`08d8690`), **1.0d** (`865f612`), **1.1** (`b9606ed`), **1.2**
  (`bbc48bf`), **1.4** (`603ac2f`), **1.7** (fLastPenEvent mutex; TSAN
  verification requires a real display). **GATE 1 stress test** (13 scenarios
  via `test_recontrol_stress.py`): TSAN single-pass = **13/13 PASS** (verified
  3× on 2026-06-10); ASAN 30-min soak = **all iterations PASS** (2026-06-10).
  Thread-lifecycle fixes this session: `omni_thread` and `CPUWorkerThread`
  converted from QThread to raw pthreads so `pthread_join` gives TSAN a proper
  happens-before point; `DestroyThread` now joins before deleting the thread
  object; `RcCmd_Load` uses a two-timer split to prevent deadlock when a Qt
  modal dialog is mid-close on the emulation thread (`BlockOnDialog` + nested
  `msgBox.exec()`). 1.5/1.6 deferred (TSAN verify requires real display).
  `phase-1-complete` tag now exists (HEAD `460449e`). Repros live in
  `tests/phase1/` (self-launching, offscreen).
- **Phase 2 — Tasks 6–8 LANDED (2026-06-10); GATE 2 (Task 9) is the only
  remaining step.** The honest input contract (delivered-ACK + truthful drop
  errors, commit `e177361`, Tasks 6+7) and the approach-B STOP-exit `EvtWakeup`
  wake hook + loser deletion (this session, Task 8) are on master;
  `PrvWakeUpCPU` and the 2026-03-13 poll-always patch are deleted (R2/2.3).
  Landmine #3 is FIXED (see Landmines). Verified on master: honest-ACK + speed
  + phase-1 repros 7/7 PASS, idle delivery 8/8 100% (p50 234 ms), app-switch
  repro 3/3 clean (2,400 switches). **GATE 2 Step 3 (TSAN race-check of the new
  delivery machinery) RUN & PASSING (2026-06-11):** the `fDeliveryLock`/condition
  + `Wait{Key,Pen}Delivery` cross-thread path and the STOP-exit `EvtWakeup` hook
  are both TSAN-clean (0 implicating reports across 5 stress runs on a
  freshly-rebuilt `build-tsan`). En route this surfaced — and fixed — a TSAN
  blind spot: `QWaitCondition::wait` in un-instrumented `libQt6Core` hides the
  mutex hand-off, so TSAN emitted **4 false** delivery-machinery reports
  (double-lock + 2× lock-order-inversion + a "race" on `fPenDeliveredSeq` where
  both threads provably held the same `fDeliveryLock`). Fix: annotate
  `omni_condition::wait`/`timedwait` with `QtTsan::mutex*` (this commit; no-op
  outside TSAN — see architecture.md Threading note). Result: delivery-machinery
  reports **4 → 0** across 3 re-runs, #5/#6 baseline unchanged (67–82/run, in the
  documented range), `test_honest_ack` PASS + delivery 10/10 100% on the normal
  build. Residuals (not blockers, not the new code): `load_during_queue` exit-66
  is the documented libtsan `try_emplace` abort (Qt threadpool JPEG-decode during
  `load`; see Landmines #11 + `EmSession.cpp:753-757`), flaky ~1/3 → 11/13 when it
  fires else 13/13; and one pre-existing double-lock in `CPUWorkerThread`'s
  hand-rolled `QMutex`+`QWaitCondition` (same QWaitCondition class, annotatable
  later by the same pattern). **Still NOT run — GATE 2 Steps 1/2/4/5:** 200-tap
  delivery matrix ≥99% at 1x+Max, idle-CPU medians, one-wake-mechanism grep gate,
  and the `phase-2-complete` tag. Execution history below.
- **Phase 2 — COMPLETE (GATE 2 PASSED 2026-06-11).** Task 1 done
  (commit `5cd5c62`: `speed [<percent>|max]` ReControl command + a stale
  `fEmulationSpeed` comment fix). Then the R1 measurements **overturned the
  plan's central assumption** and the plan was **revised in place** (user
  choice: "re-plan before coding"):
  - **Verified baseline:** delivery to an idle guest = **0%** (guest sleeps in
    `evtWaitForever`; tap/key/button/`launch` all fail to wake it; idle CPU
    ≈46% at 1x). The plan's `button app1 tap`→Datebook bootstrap is unworkable
    at idle. See landmine #3 above and handoff **§11**.
  - **Corrected mechanism logic:** robust B (STOP-exit `EvtWakeup` hook) is
    **effectively mandatory** — it is the only path that wakes a cold-asleep
    guest *and* the only way to bootstrap any in-app test. Approach **A cannot
    wake an already-asleep guest** (it lives only in the `PuppetString`
    headpatch), so A is demoted to an optional awake-path optimisation, not a
    delivery candidate or fallback.
  - **Revised order:** approach-B experiment FIRST (bootstrap + make-or-break)
    → delivery test built on the B branch, retargeted at the idle→deliver
    effect → A idle-cost measurement (demoted question) → checkpoint
    ("B+C" vs "B+C+A-no-sleep") → honesty plumbing/contract/land+delete/GATE 2.
  - Still-valid decisions (handoff §10): Q-ACK `OK delivered`, Q-DROP
    status-returning posts, Q-SYNC delivery counters, Q-IDLE /proc harness,
    Q-SPEED `speed` (done), Q-DEV m515, Q-CLEAN delete `PrvWakeUpCPU`.
  Plan + revision banner:
  `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md`; corrected
  ground-truth: handoff §11
  (`docs/superpowers/plans/2026-06-10-phase2-planning-handoff.md`).
  - **Approach-B experiment PASSED (2026-06-10, branch `phase2-experiment-B`
    @ `ad9d029`) — with a second baseline correction (handoff §12).**
    The make-or-break gate (idle launcher → tap Date Book icon → Datebook
    opens) passed on boot, loaded-session, and key paths. Delivery matrix
    (100 taps/run, healthy psf): **idle/1x 100/100 (p50 220 ms)**, idle/max
    100/100 (53 ms), rapid/max 100/100 (52 ms), rapid/1x 90/100 — truncated
    by pre-existing landmine **#10**, zero soft failures. Phase-1 repros
    7/7 PASS; speed cmd PASS. TSAN stress: 13/13 ×2 + one 11/13 (landmine
    #11b); **no report implicates the hook/delivery queues** — all reports
    are #5/#6 family, equally present on master (49–67/run, incl. an
    `ExecuteStoppedLoop`-frame report with no hook in the binary). Idle CPU
    1x: B 80.57% vs master 80.50% (3-run medians) — hook cost ≈ 0.
    En route, §11's mechanism story was **overturned**: the old psf was
    wedged (see "Session-file baseline" below); true master baseline on a
    healthy psf DELIVERS at idle via app polling (~300 ms p50, resolves
    Q-B4). **NEXT: the 2.2 checkpoint decision is "B + C" vs
    "natural-delivery + C"** (A stays dead): B = guaranteed ≤1-tick bound +
    measured latency win + zero measured cost; natural = no new mechanism,
    relies on apps polling. Session break here (R6).
  - **GATE 2 PASSED (2026-06-11) — Phase 2 COMPLETE.** Task 9 all steps green on
    master (HEAD has the landed approach-B hook, healthy `m515.psf`):
    - **Step 1 — 200-tap delivery matrix:** 1x **200/200 (100%)**, Max
      **200/200 (100%)** — 0 failures across all 400 taps, every response
      `OK delivered`. End-to-end latency p50 225 ms @1x, 68 ms @Max (Max ≈3.3×
      faster confirms `speed` drives the throttle, not just accepted). Well
      above the ≥99% bar at both speeds.
    - **Step 2 — idle CPU, landed mechanism (3× each):** 1x median **80.65%**
      (81.02/80.65/80.62) vs baseline 80.50% → hook cost ≈ +0.15%, within noise
      (the ~80% is the healthy psf's app-polling idle, not the hook). Max median
      **99.92%** (99.92/99.92/99.70) — pegs a core by design (STOP-loop sleep is
      `speed>0`-gated; offscreen platform). Documented caveat, not a regression.
    - **Step 3 — TSAN race-check:** DONE & PASSING `7be99ad` (0 implicating
      reports across 5 stress runs).
    - **Step 4 — grep gate:** exactly one *input-delivery* wake mechanism,
      `EmCPU68K.cpp:1022` (STOP-exit `EvtWakeup`, `HasPenEvent`/`HasKeyEvent`-
      gated, CPU thread). `PrvWakeUpCPU` **deleted** — survives only as the
      comment at `EmCPU68K.cpp:1011`. Remaining `EvtWakeup` calls are the
      ROMStubs stub def + pre-existing upstream paths (file import,
      UI-interrupt, HostControl, post-load reset, gremlins) and the `launch`
      app-switch path (`ReControlCmds_Session.cpp:283`, `kCmdWorkerSysCall`,
      worker thread, ROM-syscall-safe) — none a competing pen/key wake. The
      `src/Emulator_Src_3.5/` hits are the reference tree (not compiled).
    Tagged `phase-2-complete`. **Phase 3 IN PROGRESS — see below.**
  - **Session-file baseline (2026-06-10):** the old machine-local `m515.psf`
    (Feb 20) was saved WEDGED — guest in a supervisor ROM busy-loop near
    `HwrIRQ5Handler`, SR intmask=6 (timer interrupt masked), STOP never
    executed; no input mechanism can ever reach that state, and it produced
    §11's 0% numbers and the meaningless 46% idle-CPU figure. Preserved as
    `m515-wedged-artifact.psf` (psf files are gitignored/machine-local);
    `m515.psf` re-saved healthy (launcher, All category, calibrated). Cause
    of the wedge = open question. **Other machines must re-create a healthy
    psf** (boot ROM → calibrate → save) — psf files do not travel via git.

- **Phase 3 — COMPLETE (GATE 3 PASSED 2026-06-12, tagged
  `phase-3-complete`).**
  - **Plan 3a COMPLETE** (tasks 3.1 + 3.4, final commit `239fe22`): proxy
    rebuilt around a single 37-tool source-of-truth `kTools[]` table; full
    debug surface MCP-exposed; central argument validation (missing/bad arg →
    `ERR usage`, never silent default); Python test infra consolidated into
    `tests/lib`; SKILL.md drift test-gated. Regression sweep: surface 3/3,
    dispatch 37/37.
  - **Plan 3b COMPLETE** (tasks 3.2 + 3.3, final commit `1d4d54b`): landmine
    #1 FIXED (`break` real — `EnterDebugger` fallback raises
    Continue/Debug/Reset dialog, `test_break_real.py` 3× PASS); landmine #8
    FIXED (SLP sockets off by default, `EventCallback` stoppers bounded at
    5000ms, `repro_slp_trap.py` ALL PASS).
  - **Plan 3c landmine #7 — ROOT-FIXED 2026-06-12 (deferral overridden by
    user decision; plan `2026-06-11-landmine7-root-fix.md`, branch
    `landmine-7-root-fix`).** Phase 3c history: freeze R1-measured
    (`3edd8b3`), negative-caching fix relocated it, reverted per §C3
    (`abdb074`). This session: pre-fix reproduction on the C2+strip binary
    **ACCEPTANCE FAIL** (first2min 37.5% → last2min **99.8%** CPU, RSS
    **+98.8 MB**/10 min — episodic bursts then sustained pin). Fix = C2
    re-applied + **per-site verdict cache** (see Landmines #7 for mechanism).
    **Post-fix acceptance BOTH PASS:** ScreenAccess — flagged median 40.3%
    vs baseline 38.4%, drift 37.5→38.9, RSS +0.8 MB, probes <2 ms; all six
    DRAM flags — RSS +0.9 MB, probes <2 ms (guest parks on the first
    report's dialog — the once-per-arming contract). Suppression semantics:
    `test_check_suppression.py` 3× PASS + 1 confirm. Regression sweep clean:
    phase-1 repros 7/7, honest_ack, surface 3/3, dispatch 37/37, slp_trap,
    break_real (one round-1 dialog flake immediately after the acceptance
    teardown, then 2× consecutive ALL PASS). Per-access fprintf traces from
    `870b7ab` stripped (they polluted all prior #7 measurements).
  - **GATE 3 — RUN 2026-06-12, FAIL.** Pre-flight PASS (surface 3/3, dispatch
    37/37). Gate agent confirmed: install error self-describing, launch/UI/crash/
    inspect/`palm_dialog` query all working, `palm_reset` from `blocked_on_ui`
    working (task 1.0d confirmed), zero "Unknown tool". Two gaps blocked a PASS:
    (1) **`palm_break clearall` times out on hot ROM addresses** — the CPU
    re-hits the breakpoint before any WorkerCycle boundary after `respond=continue`;
    fix = allow clearall to operate immediately when `blocked_on_ui` (same
    pattern as task 1.0d for `palm_reset`). (2) **ROM poke is permanent** for
    the session — `respond=reset` reboots into the ILLEGAL instruction and
    crashes again; fix = gate prompt must save+restore original bytes around the
    poke. A third quality gap: `palm_load` from `blocked_on_ui` deadlocks the
    ReControl server (needs a blocked-state guard + actionable error). Full
    findings: `docs/superpowers/plans/2026-06-12-gate3-fail-findings.md`.
  - **GATE 3 gap fixes LANDED (2026-06-12, same day).** All four findings
    gaps fixed, reproduce-first: `tests/phase3/test_break_blocked_ops.py`
    failed pre-fix with the gate's exact `ERR timeout` on `break clearall`
    from `blocked_on_ui`, 3× ALL PASS post-fix. (1) `break` →
    `kCmdAdaptive` (runs directly while blocked; canonical hot-address
    cleanup is now clearall-then-continue, no re-hit) — `test_break_real.py`
    updated from its continue/clearall race-loop to the new contract.
    (2) Gate prompt (plan 3c Task C3) now saves/restores original bytes
    around the ILLEGAL poke; SKILL.md documents poke permanence.
    (3) `load` while blocked: dismiss-and-defer path REPLACED by immediate
    `ERR blocked` refusal (old path deadlocked — dismissal assumptions
    predate Phase 3b breakpoint/crash dialogs, which re-raise and re-block
    before the deferred teardown). (4) Syscall-boundary timeouts carry a
    recovery hint. Sweep clean: phase-1 7/7, honest_ack, surface 3/3,
    dispatch 37/37, slp_trap, check_suppression, break_real. Resolution
    detail: findings doc RESOLUTION section.
  - **GATE 3 — RE-RUN 2026-06-12, PASSED. Phase 3 COMPLETE, tagged
    `phase-3-complete`.** Fresh `pose64-tester` agent, revised Task C3
    prompt (save/restore bytes around the poke), fresh emulator on 6416
    from the fixed build (`c9989b6`), pre-flight surface 3/3 + dispatch
    37/37, breakpoint table verified empty. All six steps PASS: (1) install
    error self-describing, apps healthy; (2) Memo Pad launch + UI frontmost;
    (3) peek-saved bytes at `0x1008501A`, poked `0x4AFC`, crash →
    `blocked_on_ui`; (4) dialog message + inline register dump + backtrace
    captured while blocked; (5) original bytes poked back WHILE BLOCKED →
    `respond=continue` → running (no reset loop); (6) breakpoint at
    `0x100182BA` hit → backtrace while blocked → `break clearall` WHILE
    BLOCKED (`OK`) → continue → running. Zero "Unknown tool", zero
    raw-TCP/Bash fallbacks, zero emulator restarts. The two 2026-06-12 fixes
    (clearall-while-blocked, save/restore poke procedure) each exercised
    live in the passing run.
  - Full regression sweep at checkpoint (2026-06-11 `abdb074`): phase-1
    repros **7/7 PASS**; honest-ack **PASS**; surface **3/3**; dispatch
    **37/37**; repro_slp_trap **ALL PASS**; test_break_real **ALL PASS** (3
    rounds). No red.
- **Phase 4 — COMPLETE (GATE 4 PASSED 2026-06-12, tagged
  `phase-4-complete`).** Plan:
  `docs/superpowers/plans/2026-06-12-phase4-hotsync.md` (subagent-driven,
  two-stage review per task). Executed same day:
  - **Smoke harness** `tests/phase4/test_hotsync_smoke.py` — first live run
    reproduced an ~80% handshake failure (`Error read system info` ~2 s
    after attach; 1/5 passes proved the chain CAN work). Root-caused
    (three measured mechanisms — see HotSync status above + findings doc)
    and fixed at script level with the deterministic attach-before-tap
    procedure: **5/5 acceptance + 3/3 confirm, 16 databases each.**
  - **`info` serial line** (TDD, `tests/phase4/test_info_serial.py`):
    `serial=<descriptor>[ pty=/dev/pts/N]` — gated by transport type;
    `pty=` appears once the guest first opens the port and persists.
    Agent-visible via `palm_state`.
  - **`-preference` same-run fix** (TDD, `tests/phase4/test_preference_cli.py`):
    `Startup::PrvParseCommandLine` now rebuilds transports after CLI prefs
    are applied — pre-fix the flag updated the pref but the transport kept
    the prefs-file value (the pref LIED; effect-based test caught it).
  - **Docs:** `docs/hotsync.md` (verified procedure, why-this-order
    evidence, troubleshooting incl. the Log-bitmask trap: `Log*` prefs are
    bitmasks, 1=normal runs, 2=Gremlin-ONLY — `log set Serial 2` outside a
    Horde logs nothing; backwards glosses corrected in SKILL.md, the proxy
    `palm_log` description, and recontrol-protocol.md). SKILL.md gained the
    HotSync workflow.
  - **GATE 4 (fresh-agent reproduction from docs/hotsync.md alone): PASS** —
    first deterministic attempt, full 16-database listing, clean teardown.
    One gate finding folded back into the doc: one-shot `socat` pipes
    intermittently lose multi-line ReControl responses; use a persistent
    connection.
  - Regression sweep at close: phase-1 repros 7/7, honest_ack, surface 3/3,
    dispatch 37/37, break_blocked_ops, info_serial, preference_cli, smoke
    3× — all PASS.

## Working tree state (Phase 0 baseline, 2026-06-10)

Clean. The abandoned 2026-03-13 working tree was resolved by Phase 0 (recovery
plan), which froze a trustworthy baseline:
- All `fprintf`/timing debug instrumentation stripped (the touched control-plane
  files are back to their committed behavior).
- The one unverified behavior change — PuppetString's poll-always delivery
  (`kSkipROM` after enqueue + unconditional `clearTimeout`) — was carried as
  Phase 2 **approach A**, evaluated, **rejected** (cannot wake an already-asleep
  guest; floods awake apps with nilEvents), and the preserved patch file was
  **deleted** when approach **B** (the STOP-exit `EvtWakeup` hook) landed
  (Phase 2 Task 8). Landmine #3 is now FIXED — see Landmines #3 above.
- `src/cpp-mcp/` is now a registered submodule (`hkr04/cpp-mcp` @ `dc86c91`);
  `docs/architecture.md` and the audited doc set are committed — a fresh clone
  builds both binaries (verified) and includes the architecture guide.
- Audit-verified dead source removed (EmWindowUnix, omnithread backends, the
  unused UAE `cpuemu1-8.c`/`missing.c`, `jpeg_disabled.h`).

Reference trees (`abandoned/`, `pose32bit/`, `src/fltk-*`, `src/Emulator_Src_3.5/`)
remain on disk but gitignored — see `docs/reference-trees.md`.

## HotSync status

**WORKS — verified end-to-end 2026-06-12 (Phase 4, GATE 4 PASSED).**
`pilot-xfer -p /dev/pts/N -l` lists the stock m515 session's 16 databases;
5/5 + 3/3 consecutive passes of the automated reproduction
(`tests/phase4/test_hotsync_smoke.py`) plus an independent fresh-agent
reproduction from `docs/hotsync.md` alone (the GATE 4 run). The procedure
is **order-sensitive** — the naive tap-then-attach order loses a timing
race ~80% of the time. Three measured mechanisms (full evidence:
`docs/superpowers/plans/2026-06-12-phase4-findings.md`): the guest's CMP
retry volley lasts only ~1.2 s (~18 wakeups at 64 ms; wall-true m515 runs
~15× real device speed); stale wakeups queue in the pty slave buffer and
poison a late-attaching pilot-xfer (`Error read system info`); and the
modal "HotSync Problem" form (id=12000) swallows cradle re-taps until
dismissed (`tap-id 12004`). Deterministic order: sacrificial tap (creates
the persistent PTY) → dismiss Problem form → flush pty → attach pilot-xfer
→ tap cradle. Residual ~10% per-attempt delivery-phase race (RX-pump ~50 ms
quantum vs the 64 ms listen window) is retried at script level; an
emulator-side fix is spec-4.3 territory, explicitly deferred. Setup:
`-preference PortSerial=serial:pty:HotSync` (same-run effective since the
Phase 4 fix); the PTY slave path is reported by `info`/`palm_state`
(`serial=… pty=/dev/pts/N`). m500 timing caveat stands: it is the one
throttle-calibrated device (~2.66× busy-tick skew) — use m515/Vx.

## Authoritative document set

| Doc | Role |
|---|---|
| `docs/STATUS.md` | This file — current truth |
| `docs/architecture.md` | Threading model, event delivery, Do-Not-Do list |
| `docs/recontrol-protocol.md` | Every ReControl command + warnings |
| `claude/skills/palm-dev/SKILL.md` | MCP tool usage for agents |
| `claude/agents/pose64-tester.md` | Autonomous tester agent |
| `docs/debugging-guide.md` | Host-side debugging (ASAN/GDB/perf) |
| `docs/recovery-plan-2026-06.md` | The active roadmap + current-position banner |
| `docs/hotsync.md` | Verified HotSync procedure (GATE 4, 2026-06-12) |
| `docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md` | historical — 1.0d complete |
| `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` | historical — Phase 1 detailed plan (GATE 1 passed) |
| `docs/superpowers/plans/2026-06-10-phase2-planning-handoff.md` | historical — Phase 2 handoff; §10 = decisions record |
| `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md` | historical — Phase 2 plan (GATE 2 PASSED) |
| `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md` | historical — Phase 3 approved spec |
| `docs/superpowers/plans/2026-06-11-phase3a-mcp-surface.md` | historical — Plan 3a complete |
| `docs/superpowers/plans/2026-06-11-phase3b-debugger-fixes.md` | historical — Plan 3b complete |
| `docs/superpowers/plans/2026-06-11-phase3c-metamemory-gate3.md` | historical — Plan 3c complete; GATE 3 PASSED 2026-06-12 (Task C3 prompt as revised 2026-06-12) |
| `docs/superpowers/plans/2026-06-11-landmine7-root-fix.md` | historical — landmine #7 root fix complete (on master) |
| `docs/superpowers/plans/2026-06-12-gate3-fail-findings.md` | historical — GATE 3 first-run FAIL findings + RESOLUTION (all gaps fixed, re-run PASSED) |
| `docs/superpowers/plans/2026-06-12-phase4-hotsync.md` | historical — Phase 4 plan complete; GATE 4 PASSED 2026-06-12 |
| `docs/superpowers/plans/2026-06-12-phase4-findings.md` | historical — HotSync race root-cause record (still the evidence behind docs/hotsync.md) |

Historical (dated, possibly wrong about today): everything in
`docs/history/`, `docs/ReControlPostMortem/` (predecessor project "RePOSE4"),
`docs/plans/`, plus `docs/debugging-infrastructure.md`
and `docs/qt-port-architectural-review.md` (banner-annotated in place),
timer/benchmark/winuae docs (accurate but point-in-time).
`docs/superpowers/` is now all historical records (Phases 1–3 complete);
the 2026-06-10 dialog finding carries a verification addendum that corrects
its hang claim. Phase 4 gets its own plan when its session starts.
