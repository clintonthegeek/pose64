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

## Recovery progress (read `docs/recovery-plan-2026-06.md` for the roadmap)

- **Phase 0 — complete 2026-06-10** (GATE 0 passed; details below).
- **Phase 1 — in progress.** Done & verified: **1.8** (`5f5c443`), **1.3**
  (`08d8690`), **1.0d** (`865f612`), **1.1** (`b9606ed`), **1.2** (this
  commit — bounded `ERR timeout` for `kCmdWorkerCycle`/`kCmdAdaptive`).
  **Next task: 1.4** (CPUWorkerThread bounded shutdown) or **1.7**
  (PostPenEvent race), per
  `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md`. Repros
  live in `tests/phase1/` (self-launching, offscreen).

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
| `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` | ACTIVE — Phase 1 detailed plan (next: 1.1) |

Historical (dated, possibly wrong about today): everything in
`docs/history/`, `docs/ReControlPostMortem/` (predecessor project "RePOSE4"),
`docs/plans/`, plus `docs/debugging-infrastructure.md`
and `docs/qt-port-architectural-review.md` (banner-annotated in place),
timer/benchmark/winuae docs (accurate but point-in-time).
`docs/superpowers/` is mixed: the **2026-06-10 plans listed above are ACTIVE**;
findings are dated records (the 2026-06-10 dialog finding carries a
verification addendum that corrects its hang claim); everything older is
historical.
