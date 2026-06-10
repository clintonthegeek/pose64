# POSE64 — Project Status

**Date:** 2026-06-09 (full code + docs audit; previous activity 2026-04-03)
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
2. **Suspend-counter leak ⇒ unrecoverable freeze.** `SuspendThread(kStopNow/
   kStopOnCycle)` increments `fSuspendByUIThread` before checking state and
   does NOT decrement on failure (`EmSession.cpp:788-793, 961-1001`). Issuing
   a `kCmdWorkerCycle` command (`ui`, `tap-id`, `break`, `watch`, `spy`)
   while a dialog is up leaks the counter; the CPU then parks forever and
   `ForceReset` deliberately skips that counter (`EmSession.cpp:2277-2285`).
   Only a process restart recovers. This is the inherited POSE 3.5 bug class
   that caused the "permanently locked emulation" reports.
3. **Input is fire-and-forget.** `tap/pen/key/type/button` return `OK` when
   queued, not when delivered; delivery depends on PuppetString firing inside
   `SysEvGroupWait`, and events can sit undelivered (guest asleep) or be
   silently dropped (Gremlins active). Argument errors are also swallowed
   (`QueueWork` always replies `OK`, `ReControl.cpp:212-218`). The uncommitted
   working-tree changes are a half-finished fix for exactly this.
4. **Untimed stops can wedge the whole control plane.** `kStopNow`/
   `kStopOnCycle` stoppers have no timeout (`EmSession.cpp:830` — timeout
   applies only to `kStopOnSysCall`), and the MCP proxy has no socket read
   timeout (`pose64-mcp-proxy.cpp:99-112`), so one wedged command hangs every
   subsequent MCP call. The proxy also auto-retries after reconnect, which
   can double-execute non-idempotent commands (`install`).
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

## Working tree state (as of this audit)

~465 uncommitted lines from the final 2026-03-13 session: mostly `fprintf`
debug instrumentation (CPUWorkerThread, EmSession, ReControl, EmPatchMgr,
EmApplication, EmWindow) wrapped around one real, plausible-but-unverified
behavior change: PuppetString force-feeds nil events + `kSkipROM` after
enqueueing pen/key and sets `clearTimeout = true` unconditionally in
interactive mode (poll-always event delivery, compensating for the removed
`PrvWakeUpCPU` wakeup). Also staged: 0.9.1 metainfo notes, doc expansions.
**Decision needed**: strip the printf noise, keep/verify the PuppetString
change. See the recovery plan.

Also: `src/cpp-mcp/` (a build dependency of the proxy) and
`docs/architecture.md` (the best doc in the repo) are **untracked** — a fresh
clone neither builds nor includes the architecture guide.

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
| `docs/recovery-plan-2026-06.md` | How development gets back on course |

Historical (dated, possibly wrong about today): everything in
`docs/history/`, `docs/ReControlPostMortem/` (predecessor project "RePOSE4"),
`docs/plans/`, `docs/superpowers/`, plus `docs/debugging-infrastructure.md`
and `docs/qt-port-architectural-review.md` (banner-annotated in place),
timer/benchmark/winuae docs (accurate but point-in-time).
