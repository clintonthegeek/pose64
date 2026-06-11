# POSE64 — Project Status

**Date:** 2026-06-09 (full code + docs audit; previous activity 2026-04-03);
Phase 1 progress updates 2026-06-10
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
  assignments were audited command-by-command and are correct.
- **MCP proxy**: exactly **28** `palm_*` tools (no debugging tools — see
  Landmines). Out-of-process design is sound.
- **Debug surface that works today**: `peek/poke/regs/backtrace`,
  `screenshot` (+scale/grid/annotate/crosshair overlays), `screen-hash`,
  `ui`, `watch`/`spy` (raise dialogs), `log` (20 categories), `gremlin`,
  `errorhandling`, `profile` (full Metrowerks .mwp output), session
  save/load, `dialog` query/respond with register dump.
- **Serial**: a full PTY transport (`pty:HotSync` in Preferences) is built in
  and prints its `/dev/pts/N` path at startup; cradle button is scriptable.

## Landmines (verified, with mechanism)

1. **`break` is passive without an external debugger.** A breakpoint hit only
   suspends the CPU if a Palm-Debugger SLP client is attached (ports
   6414/2000); otherwise it silently continues (`DebugMgr.cpp`,
   `ConditionalBreak`/`EnterDebugger`). No hit notification, no `continue`
   command. Use `watch`/`spy` instead.
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
3. **Input is fire-and-forget.** `tap/pen/key/type/button` return `OK` when
   queued, not when delivered; delivery depends on PuppetString firing inside
   `SysEvGroupWait`, and events can sit undelivered (guest asleep) or be
   silently dropped (Gremlins active). Stands until Phase 2 (the 2026-03-13
   half-finished fix is preserved at
   `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`).
   *Partially fixed 2026-06-10 (task 1.8, commit `5f5c443`):* argument errors
   are no longer swallowed — WorkerDirect args are validated on the main
   thread, so `tap banana` returns `ERR usage…`, not `OK`. Verified:
   `tests/phase1/repro_1_8_argval.py`.
   ~~**Quantified 2026-06-10 (Phase 2 execution session, HEAD `5cd5c62`):**
   delivery to an idle guest is 0%…~~ **CORRECTED later the same day
   (approach-B experiment, handoff §12):** the 0%-idle measurement was an
   artifact of a **wedged session file** — the old `m515.psf` was saved with
   the guest spinning in a supervisor ROM busy-loop, SR interrupt mask 6
   (timer blocked), never executing STOP — a state NO wake mechanism can
   reach (see "Session-file baseline" below). **True baseline (master,
   healthy psf): idle taps DELIVER** (~300 ms p50) because built-in apps
   poll `EvtGetEvent` with finite timeouts. The honest residual gap: apps
   that genuinely use `evtWaitForever` get input only when something wakes
   them — the Phase-2 approach-B hook (experiment passed, branch
   `phase2-experiment-B`) provides a guaranteed ≤1-tick wake; the 2.2
   checkpoint decision ("B + C" vs "natural + C") is open. Detail: handoff
   §11 (measurements) + **§12 (corrections + experiment results)**.
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
7. **`check set` re-arms a known freeze.** Any DRAM-region check flag
   restores an O(n) heap scan per memory access — 100% CPU within ~10 min
   (bypassed by default in `0bc2a41`, never fixed).
8. **SLP debugger sockets listen by default** (6414/2000) and connecting
   triggers an untimed main-thread `kStopOnSysCall` stop
   (`Debug::EventCallback`) — a UI hang waiting to happen.
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
- **Phase 2 — IN PROGRESS (2026-06-10 execution session).** Task 1 done
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
  - **Session-file baseline (2026-06-10):** the old machine-local `m515.psf`
    (Feb 20) was saved WEDGED — guest in a supervisor ROM busy-loop near
    `HwrIRQ5Handler`, SR intmask=6 (timer interrupt masked), STOP never
    executed; no input mechanism can ever reach that state, and it produced
    §11's 0% numbers and the meaningless 46% idle-CPU figure. Preserved as
    `m515-wedged-artifact.psf` (psf files are gitignored/machine-local);
    `m515.psf` re-saved healthy (launcher, All category, calibrated). Cause
    of the wedge = open question. **Other machines must re-create a healthy
    psf** (boot ROM → calibrate → save) — psf files do not travel via git.

## Working tree state (Phase 0 baseline, 2026-06-10)

Clean. The abandoned 2026-03-13 working tree was resolved by Phase 0 (recovery
plan), which froze a trustworthy baseline:
- All `fprintf`/timing debug instrumentation stripped (the touched control-plane
  files are back to their committed behavior).
- The one unverified behavior change — PuppetString's poll-always delivery
  (`kSkipROM` after enqueue + unconditional `clearTimeout`) — was **deferred to
  Phase 2**, not adopted. It is preserved at
  `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`
  (landmine #3 stands until Phase 2 verifies a fix).
- `src/cpp-mcp/` is now a registered submodule (`hkr04/cpp-mcp` @ `dc86c91`);
  `docs/architecture.md` and the audited doc set are committed — a fresh clone
  builds both binaries (verified) and includes the architecture guide.
- Audit-verified dead source removed (EmWindowUnix, omnithread backends, the
  unused UAE `cpuemu1-8.c`/`missing.c`, `jpeg_disabled.h`).

Reference trees (`abandoned/`, `pose32bit/`, `src/fltk-*`, `src/Emulator_Src_3.5/`)
remain on disk but gitignored — see `docs/reference-trees.md`.

## HotSync status

Closer than assumed. All mechanisms exist (PTY transport, wall-clock 1x,
accurate timers, scriptable cradle). **The end-to-end smoke test has never
been run**: start emulator with Serial Port = `pty:HotSync`, note the
`/dev/pts/N` line, run `pilot-xfer -p /dev/pts/N -l`, tap the cradle button.
Known risk: on the calibrated m500, PalmOS ticks run ~2.66× fast during
CPU-busy stretches (calibration corrects the throttle, not the timer) — test
on an uncalibrated device (Palm V/Vx) first, where ticks stay wall-true.

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
| `docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md` | historical — 1.0d complete |
| `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` | historical — Phase 1 detailed plan (GATE 1 passed) |
| `docs/superpowers/plans/2026-06-10-phase2-planning-handoff.md` | ACTIVE — Phase 2 handoff; §10 = decisions record |
| `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md` | ACTIVE — Phase 2 implementation plan (next to execute) |

Historical (dated, possibly wrong about today): everything in
`docs/history/`, `docs/ReControlPostMortem/` (predecessor project "RePOSE4"),
`docs/plans/`, plus `docs/debugging-infrastructure.md`
and `docs/qt-port-architectural-review.md` (banner-annotated in place),
timer/benchmark/winuae docs (accurate but point-in-time).
`docs/superpowers/` is mixed: the **2026-06-10 plans listed above are ACTIVE**;
findings are dated records (the 2026-06-10 dialog finding carries a
verification addendum that corrects its hang claim); everything older is
historical.
