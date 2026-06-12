# POSE64 Recovery Plan

> **For agentic workers:** This is the strategic roadmap. Each phase below
> MUST get its own detailed implementation plan (via superpowers:writing-plans,
> saved to `docs/superpowers/plans/`) at execution time, when the executing
> session has the live code in context. Do not execute phases out of order;
> the gates exist because this project previously died of skipped gates.

> **CURRENT POSITION (updated 2026-06-12 — keep this banner current, R5):**
> Phase 0 **complete** (GATE 0 passed). Phase 1 tasks **1.8, 1.3, 1.0d,
> 1.1, 1.2, 1.4, 1.7** all done & verified. **GATE 1 PASSED (2026-06-10):**
> TSAN single-pass 13/13 PASS (verified 3×); ASAN 30-min soak all iterations
> PASS. `phase-1-complete` **tag exists** (HEAD `460449e`).
> **Phase 2 COMPLETE — GATE 2 PASSED (2026-06-11).** Honest input contract
> landed (`e177361`), approach-B STOP-exit `EvtWakeup` hook on master,
> `PrvWakeUpCPU`/approach-A deleted (R2). 200-tap matrix 200/200 (100%) at 1x
> and Max (0/400 fails); idle CPU 1x median 80.65% vs baseline 80.50% (hook
> cost ≈ 0); TSAN-clean (0 implicating reports); grep gate clean — one
> input-delivery wake (`EmCPU68K.cpp:1022`). Tagged `phase-2-complete`.
> **Phase 3 IN PROGRESS (NOT yet certified complete) — expanded scope per**
> `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`,
> **executed as three plans (checkpoint HEAD `abdb074`):**
> - **Plan 3a (MCP surface) — COMPLETE:** proxy rebuilt around a single
>   37-tool source-of-truth table; full debug surface MCP-exposed
>   (backtrace/break/watch/spy/log/gremlin/check/errorhandling/profile/speed);
>   central argument validation; drift test-gated. Tasks 3.1 and 3.4 complete.
>   Final commit `239fe22`.
> - **Plan 3b (debugger fixes) — COMPLETE:** tasks 3.2 (`break` real — landmine
>   #1 FIXED: `EnterDebugger` fallback schedules `EmDeferredErrBreakpoint` →
>   Continue/Debug/Reset dialog; repro `tests/phase3/test_break_real.py` 3×
>   PASS) and 3.3 (SLP trap — landmine #8 FIXED: `CreateListeningSockets`
>   gated behind `SLPDebugger` pref + `--slp-debugger` flag; both
>   `EventCallback` stoppers bounded at 5000ms; repro
>   `tests/phase3/repro_slp_trap.py` ALL PASS). Final commit `1d4d54b`.
> - **Plan 3c (MetaMemory + GATE 3) — landmine #7 ROOT-FIXED 2026-06-12
>   (deferral overridden by user decision; plan
>   `2026-06-11-landmine7-root-fix.md`):** pre-fix reproduction on the
>   C2+strip binary FAILED acceptance exactly as Phase 3c measured (first2min
>   37.5% → last2min 99.8% CPU, RSS +98.8 MB/10 min). Fix = C2 re-applied
>   (negative caching + dedup) **plus a per-site verdict cache** at the
>   `ProbableCause` boundary — each (PC, size, meta-sig, r/w) site analyzed/
>   reported once per arming, repeats suppressed; generation + chunk-anchored
>   invalidation; dynamic-context forgiveness never cached. Reporting
>   semantics now once-per-site-per-arming (documented). Evidence:
>   `test_check_suppression.py` 3× PASS; post-fix acceptance both runs (see
>   STATUS). Per-access fprintf traces from `870b7ab` stripped en route.
> - **GATE 3 — RUN 2026-06-12, FAIL.** Pre-flight PASS; zero "Unknown tool";
>   install/launch/crash/inspect all confirmed working. Two gaps blocked the
>   pass: **(1)** `palm_break clearall` times out on hot ROM addresses —
>   WorkerCycle boundary never arrives because the CPU re-hits the breakpoint
>   immediately after every continue; fix = allow clearall to operate directly
>   when `blocked_on_ui` (same pattern as task 1.0d for `palm_reset`).
>   **(2)** ROM poke is permanent for the session — `respond=reset` reboots
>   into the ILLEGAL instruction and loops; fix = gate prompt must
>   save+restore original bytes around the poke. Full findings and fix spec:
>   `docs/superpowers/plans/2026-06-12-gate3-fail-findings.md`.
>   **Phase 3 is NOT certified complete.**
>
> **NEXT ACTION:** Fix Gap 1 (clearall from `blocked_on_ui`, emulator code
> change, task-1.0d pattern — reproduce-first per R1), fix Gate 3 prompt
> (save+restore bytes around poke), re-run GATE 3. On PASS: tag
> `phase-3-complete` and proceed to Phase 4. Do NOT tag before the live GATE 3
> run passes.

**Goal:** Take POSE64 from "abandoned mid-debug, unstable under automation"
to "stable, honest, useful for AI-driven Palm reverse engineering, with one
demonstrated HotSync" — the project's definition of *done* (v1.0).

**Architecture:** Stabilization-first. Fix the four verified freeze/lying
mechanisms in the control plane before adding ANY new feature. Every fix
lands with a failing reproduction first. One event-delivery mechanism survives;
all superseded mechanisms are deleted, not stranded.

**Tech stack:** Qt6/C++17, CMake, ASAN/TSAN, Python test harness
(`test_recontrol_stress.py`), pilot-link for the HotSync milestone.

**Source of truth for findings:** `docs/STATUS.md` (audited 2026-06-09).

---

## Why the project derailed (so we don't repeat it)

From the seven-agent audit of code, git history, and docs:

1. **One bug, six fixes, six places.** Cross-thread CPU wakeup was patched
   where it *manifested*, never where it originated (7fe4e1b → c07a464 →
   0fad58d → 7981631 → 7b51f89 → 948aa09; median fix-to-fix ~70 minutes).
2. **Mechanisms stacked, never replaced.** 18 `EmSessionStopper` call sites
   coexist with the worker queue meant to replace them; the 7-category
   dispatch table codified the layering.
3. **Docs declared victory before verification.** "100% complete" obsolete in
   34 minutes; "all real, all tested" disproven the same afternoon; "proxy
   auto-discovers tools" was never true and seeded phantom `palm_*` tools
   that sent later agents chasing ghosts.
4. **Tests asserted protocol responses, not effects.** `launch` was broken by
   design for 22 days under a green "comprehensive integration test suite";
   `test_cpu_worker_tap.py` never checked the tap landed.
5. **Giant unverified sessions.** 28 commits in 3h40m overnight (mostly
   Haiku-authored, Opus-repaired); a 19-hour final day ending in an
   uncommitted instrumentation dump.

## Process rules (binding for every session below)

- **R1 — Reproduce first.** No fix commit without a failing test or scripted
  reproduction committed alongside (or immediately before) it.
- **R2 — Replace, don't stack.** A commit that introduces mechanism B for a
  job mechanism A did must delete or explicitly quarantine A in the same
  commit.
- **R3 — Effects, not responses.** Input/launch/install tests must assert on
  emulator effects (screen-hash change, `ui` content, `apps` list), never
  only on `OK`.
- **R4 — Clean tree per session.** End every session with commit-or-revert.
  Debug instrumentation never survives a session. (`git stash` is not an
  archive.)
- **R5 — Docs in the same commit.** A behavior change to ReControl/MCP lands
  with `docs/recontrol-protocol.md` + `SKILL.md` updates in the same commit.
  `docs/STATUS.md` is updated only with *verified* statements.
- **R6 — No overnight mega-sessions; strongest model for threading work.**
  One phase-task per sitting, with the gate run before stopping.

---

## Phase 0 — Freeze a trustworthy baseline (half a day)

> **COMPLETE 2026-06-10, GATE 0 PASSED** (fresh-clone build OK; m515 ROM
> boots; `state` answers on 6416). The PuppetString candidate was resolved
> via the "unverifiable in one sitting" branch: checked out away and preserved
> at `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`
> for Phase 2. Execution record:
> `docs/superpowers/plans/2026-06-09-phase-0-trustworthy-baseline.md`.

**Outcome:** a clean, buildable-from-fresh-clone repo; the March-13 working
tree resolved deliberately instead of by accident.

### Task 0.1 — Triage the uncommitted working tree
**Files:** the 12 modified files (`git diff --stat`); see STATUS.md.
- [x] Strip ALL `fprintf` instrumentation from: `CPUWorkerThread.cpp`,
      `EmSession.cpp`, `EmApplication.cpp`, `EmWindow.cpp`, `ReControl.cpp`,
      `EmApplicationQt.cpp`, `Patches/EmPatchMgr.cpp` (keep behavior changes,
      remove prints; the `[PuppetString]`/`[PenEvent]`/`[CPUWorker]`/SLOW
      probes all go).
- [x] Keep, as a candidate, the PuppetString change in `EmPatchMgr.cpp`
      (nil-event + `kSkipROM` after enqueue; unconditional `clearTimeout`).
      Build and run the Phase-2 delivery test (Task 2.1) once WITH and once
      WITHOUT it (`git stash`). Record results in the commit message.
- [x] If it passes: commit as `fix: deliver queued pen/key events via
      PuppetString poll (replaces PrvWakeUpCPU wakeup)`. If unverifiable in
      one sitting: `git checkout -p` it away — it is reproducible from
      STATUS.md, and an unverified hack must not be the baseline.
- [x] Commit the unambiguous keepers separately: `claude/` doc updates,
      `data/...metainfo.xml` 0.9.1 notes, `EmSPISlaveADS784x.cpp` comment.
- [x] Delete the `if (0)` Wiggle Walk block in `EmWindow.cpp:474-506` and the
      `fWiggled` machinery, or file it as a tracked issue — no zombie code.

### Task 0.2 — Make a fresh clone work
- [x] `git add src/cpp-mcp/` (the proxy build depends on it — currently
      untracked!), `docs/architecture.md`, `docs/STATUS.md`, this plan,
      `docs/history/` additions… verify with:
      `git clone . /tmp/pose64-clone && cmake -S /tmp/pose64-clone -B /tmp/pose64-clone/build && cmake --build /tmp/pose64-clone/build -j` → must succeed.
- [x] Decide `src/Emulator_Src_3.5/` (33 MB reference): recommend gitignore +
      a `docs/` note pointing to the canonical tarball. Do NOT commit.

### Task 0.3 — Repo hygiene
- [x] Append to `.gitignore`: `__pycache__/`, `.cache/`, `*.AppImage`,
      `*.deb`, `*.ddeb`, `*.exe`, `abandoned/`, `pose32bit/`,
      `src/fltk-1.1.10/`, `src/fltk-install/`, `src/core/UAE/gen/`,
      `src/Emulator_Src_3.5/`, `Screenshot_*.jpg`.
- [x] Archive elsewhere (or delete): `abandoned/` (158 MB), `pose32bit/`
      (88 MB), `src/fltk-*` (50 MB), `src/core/UAE/gen/`, `docs/thing.pdf`,
      `'c:\palm\bigclock\log.txt'`, `Screenshot_20260218_182122.jpg`.
- [x] SAFE-DELETE dead source (audit-verified unreferenced):
      `src/platform/EmWindowUnix.cpp`,
      `src/core/omnithread/{mach,nt,null_thread,posix,solaris}.*`,
      `src/core/UAE/cpuemu1.c`–`cpuemu8.c`, `src/core/UAE/missing.c`,
      `src/core/jpeg_disabled.h`.

**GATE 0:** fresh-clone build passes; `git status` is empty; `pose64` boots a
ROM and `state` answers on 6416.

---

## Phase 1 — Kill the freeze classes (2–4 sessions)

**Outcome:** an AI agent can hammer the emulator for 30 minutes — including
crash dialogs, concurrent GUI use, and client disconnects — without a freeze,
a lie, or a process restart. This phase is the project's core debt; nothing
else proceeds until GATE 1 passes.

Ranked tasks (each = failing repro → fix → test → commit):

- [x] **1.0d Dialog-action lifetime fix** *(added 2026-06-10 — landmine #9,
      discovered building the 1.1 repro)* — `EmSession::BlockOnDialog` posts an
      `EmActionDialog` referencing its own stack frame; `fReset`/`fStop` break
      the wait and let the frame die while the action is still queued (UAF
      read → SIGSEGV) or running (UAF write through `fDlgResult`). Fix: a
      queued/running/done/cancelled handshake under `fSharedLock` so
      `BlockOnDialog` never returns while the action can touch its stack.
      Repro: `tests/phase1/repro_dialog_subsystem.py` (both interleavings).
      Full plan: `docs/superpowers/plans/2026-06-10-task-1-0d-dialog-lifetime.md`.
      Goes first so `reset` is a safe recovery for the dialog-heavy 1.1/1.2
      repros. (The companion "dialog never shows" hang claim was disproven
      live the same day — dialogs show one idle tick after `blocked_on_ui`.)
- [ ] **1.1 Suspend-counter leak** — `EmSession.cpp` `SuspendThread`: the
      `kStopNow`/`kStopOnCycle` failure paths return `false` without
      decrementing `fSuspendByUIThread` (increment at :788-793, returns at
      :961-1001). Make failure side-effect-free. Repro (verified plumbing):
      `spy set 0x134` → `blocked_on_ui` within ~1 s, then issue `ui` →
      dismiss via `dialog respond continue` → `state` must return `running`
      (`spy clear` to stop re-fires). Also make `ForceReset` clear the counter
      as a belt-and-suspenders (document why). The exact fix diff (written,
      reverted unverified per R1/R4) is in
      `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` Task 1.1.
- [ ] **1.2 Universal stop timeouts** — give `kStopNow`/`kStopOnCycle` the
      same deadline treatment `kStopOnSysCall` got (`EmSession.cpp:830`);
      every `kCmdWorkerCycle`/`kCmdWorkerRaw` handler returns
      `ERR timeout: …` instead of blocking forever.
- [x] **1.3 Proxy honesty** — **DONE 2026-06-10, commit `08d8690`** —
      `SO_RCVTIMEO` added (wedged server → `ERR timeout` instead of hanging
      every later MCP call); non-idempotent commands are never
      reconnected-and-resent after a dropped response. Verified repro:
      `tests/phase1/repro_1_3_proxy.py`.
- [ ] **1.4 Worker shutdown** — `CPUWorkerThread::shutdown()` must not be
      called unbounded from the main thread (`ReControlCmds_Session.cpp:342`,
      `main.cpp:134-139`): set a stop flag under the mutex, wake, bounded
      `wait(5s)`, then escalate. Repro: `palm_load` while a slow command is
      queued.
- [ ] **1.5 Single ROM-call owner** — close the two-stoppers window
      (`EmSession.cpp:979-986`): a global ROM-call mutex (or owner token)
      around `ExecuteSubroutine`/`CallCPU` so a GUI menu action and a worker
      command can never both run the UAE core. Repro is probabilistic — add a
      TSAN job and an adversarial stress scenario (GUI-equivalent calls
      interleaved with worker commands).
- [ ] **1.6 PaintScreen stopper** — restore CPU-stop (original POSE behavior)
      around `EmWindow::PaintScreen`'s `EmScreen::GetBits`, or make
      `gMemAccessFlags`/`CEnableFullAccess` thread-safe. The "torn read is
      acceptable" comment is wrong about what's being torn.
- [ ] **1.7 `fLastPenEvent` race** — two writer threads
      (`EmSession.cpp:1909-1935`); protect with the queue's mutex or an
      atomic.
- [x] **1.8 WorkerDirect error reporting** — **DONE 2026-06-10, commit
      `5f5c443`** — args validated on the main thread before queueing
      (`RcCommandEntry::validate` + `RcValidate_*`); `tap banana`,
      `tap 5 banana`, `key abc` etc. now return `ERR usage…` immediately.
      Verified repro: `tests/phase1/repro_1_8_argval.py`.

**GATE 1:** extended `test_recontrol_stress.py` (add scenarios: commands
while dialog pending; load-during-queue; disconnect storms; 2-client `ERR
busy` cycling; concurrent screenshot+menu) runs 30 minutes clean under ASAN,
and the suite passes under TSAN with the new lock discipline. All assertions
are effect-based (R3).

---

## Phase 2 — Make input delivery honest (1–2 sessions)

**Outcome:** `OK` from `tap`/`key`/`type` means *delivered* (or you get a
truthful error), with ONE delivery mechanism in the tree.

- [x] **2.1 Delivery test first** — **DONE.** `tests/phase2/test_delivery.py`
      (revised, effect-based: idle launcher → `tap` Date Book icon → Datebook
      form confirmed via `ui`; restore with `key 264`). Built on the experiment
      branch, **brought to master in Task 8** before that branch was deleted.
      It is the GATE 2 referee (Task 9). Shakeout on master with the landed
      hook: idle 8/8 100% (p50 234 ms).
- [x] **2.2 Decide the mechanism** — **DECIDED 2026-06-10: B + C**
      (data-backed; recorded in `architecture.md` "Phase 2 decisions" + handoff
      §10 Q-MECH RESOLVED + §12.3). B = STOP-exit `EvtWakeup` hook; A is
      rejected/dead (handoff §11.3); chosen over natural-delivery-only because
      only B bounds delivery to ≤1 tick for true-`evtWaitForever` apps. Hook
      lands + losers deleted (R2) in plan Task 8. *(single decision, recorded
      in architecture.md):*
      - **A. Poll-always** (the uncommitted PuppetString approach):
        guest never sleeps on infinite timeout; simplest; costs idle CPU —
        measure it (the sleep-until-interrupt work was a headline feature;
        don't silently destroy it).
      - **B. Targeted wake**: on enqueue, schedule a one-shot "clear timeout
        on next SysEvGroupWait" via the existing patch state, letting the
        guest sleep otherwise. More surgical; needs care vs. EvtMgrIdle.
      - **C. Delivered-ACK**: keep either A or B, and additionally have the
        worker block (≤2 s) on a delivery counter PuppetString increments, so
        the protocol response reflects truth (`OK delivered` / `ERR pending`).
      Recommendation: **B + C**. A is acceptable as an interim if measured
      idle cost is negligible at 1x.
- [x] **2.3 Delete the losers** (R2): **DONE (Task 8).** `PrvWakeUpCPU` (decl,
      body, dead `ROMStubs.h` include) deleted; the 2026-03-13 poll-always
      patch file (approach A) `git rm`'d; the `phase2-experiment-B` branch
      deleted (its unique content — the hook + `test_delivery.py` — landed on
      master first). Stale "bridge thread" comments reworded to "UI thread"
      (`EmWindow.cpp`, `CPUWorkerThread.h`). One input-delivery wake mechanism
      remains (the STOP-exit hook).
- [x] **2.4 Report drops** — **DONE (commit `e177361`, Tasks 6+7).** Pen/key
      posts return `EmPostInputResult`; ReControl maps refusals to
      `ERR busy: gremlin running` / `ERR busy: event playback active` /
      `ERR busy: minimization active` / `ERR duplicate: …`, never a swallowed
      `OK`. `button` reports `ERR busy: gremlin or playback active` too.

**GATE 2:** delivery test ≥ 99% over 200 taps at 1x and at Max speed; idle
CPU% recorded in STATUS.md; exactly one wake mechanism greppable in src/.

> **GATE-2 test scope (decided 2026-06-10, REVISED same day):** landmine
> #10 (app-switch churn → `MemoryMgr` fatal alert; STATUS #10, repro
> `tests/phase2/repro_appswitch_memmgr.py`) is **fixed in-path, before
> Task 6** (**DONE 2026-06-10** — root cause was nested-ROM-call abortion
> on `kStopNow`-family suspends, not the tailpatch itself; see STATUS #10)
> — the user rejected designing the gate around a known crash;
> certifying "stable under automation" while a known guest-killer sits in
> the agent workload would narrow the claim. GATE 2 runs BOTH rapid
> variants: the within-app Datebook Go-To/Cancel toggle (pure delivery
> referee — isolates delivery failures from survival failures) AND an
> app-switch churn run (survival under the real agent workload). Pass bar
> unchanged (≥99% delivery); the survival run must complete with no guest
> fatal alert.

---

## Phase 3 — Close the tool/doc gap (expanded scope; three plans)

**Outcome:** what agents see is what exists. (Docs were already corrected on
2026-06-09 to describe reality; this phase upgrades reality where it's worth
it.)

**Expanded scope:** per `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`,
Phase 3 is executed as three plans: **3a** (MCP surface rebuild), **3b**
(debugger fixes), **3c** (MetaMemory + GATE 3).

- [x] **3.1 Expose debug commands as MCP tools** — **DONE (plan 3a,
      `docs/superpowers/plans/2026-06-11-phase3a-mcp-surface.md`).** Proxy
      rebuilt around a single 37-tool source-of-truth `kTools[]` table;
      10 new debug-surface tools added (`palm_speed`, `palm_backtrace`,
      `palm_break`, `palm_watch`, `palm_spy`, `palm_log`, `palm_gremlin`,
      `palm_check`, `palm_errorhandling`, `palm_profile`); `palm_dbs` absorbed
      into `palm_apps all=true`; central argument validation; SKILL.md drift
      test-gated (`tests/phase3/test_mcp_surface.py`). SKILL.md +
      recontrol-protocol.md updated same commit (R5).
- [x] **3.2 Make `break` real** — **DONE (plan 3b, landmine #1 FIXED).**
      `EnterDebugger`'s no-debugger fallback now schedules
      `EmDeferredErrBreakpoint` → `Errors::ReportErrBreakpoint` →
      Continue/Debug/Reset dialog (`blocked_on_ui`), inspectable via `dialog`
      and `backtrace`, resumable via `dialog respond continue`. Same deferred-
      error path `watch`/`spy` use. Repro: `tests/phase3/test_break_real.py`
      3x PASS (each run = 3 rounds). **Plan 3b:**
      `docs/superpowers/plans/2026-06-11-phase3b-debugger-fixes.md`
- [x] **3.3 Defuse the SLP trap** — **DONE (plan 3b, landmine #8 FIXED).**
      Debugger sockets (6414/2000) off by default; `CreateListeningSockets`
      gated behind `SLPDebugger` bool pref (default false) and
      `--slp-debugger` CLI flag. Both `EventCallback` stoppers bounded at
      5000ms with graceful skip log. Repro:
      `tests/phase3/repro_slp_trap.py` ALL PASS (default-off + opt-in
      port-accepts + control-plane survival through connect/disconnect).
      **Plan 3b:**
      `docs/superpowers/plans/2026-06-11-phase3b-debugger-fixes.md`
- [x] **3.4 Consolidate Python clients** — **DONE (plan 3a).** Scratch scripts
      (`datebook_interaction.py`, `garak_intrigue.py`, `test_cpu_worker_tap.py`)
      deleted; `ReControlClient` + harness promoted to `tests/lib`; phase-1
      repros re-export from there; `tests/test_cpu_worker_tap.py` ported.
- [ ] **3.5a Landmine #7 root fix** — DEFERRED to POST-V1 (spec §C3 fallback;
      see "What we are explicitly NOT doing" below and STATUS.md #7 for full
      evidence). `palm_check` ships with a truthful measured warning.
      **Plan 3c:** `docs/superpowers/plans/2026-06-11-phase3c-metamemory-gate3.md`
- [ ] **3.5b GATE 3 — PENDING** — fresh-agent MCP run: SKILL.md only, install
      → launch → crash → inspect (backtrace via MCP) → recover, zero "Unknown
      tool", zero raw-TCP fallbacks. Prerequisite: pose64 MCP server reconnected
      to the rebuilt 37-tool proxy + emulator on port 6416. **On PASS: tag
      `phase-3-complete` and proceed to Phase 4.**

**GATE 3:** a fresh agent given only SKILL.md completes install → launch →
crash → inspect (backtrace via MCP) → recover, with zero "Unknown tool" and
zero raw-TCP fallbacks.

---

## Phase 4 — The HotSync milestone (1 session + contingency)

**Outcome:** one documented end-to-end HotSync against pilot-link — the
project's reason to exist beyond parity. All mechanisms already shipped;
nobody ever ran the test.

- [ ] **4.1 Smoke test** (do this FIRST; it may just work):
      1. Session on an **uncalibrated** device (Palm V/Vx ROM — wall-true
         ticks; avoids the m500's 2.66× busy-tick skew).
      2. Preferences → Serial Port = `pty:HotSync`; note stderr line
         `connect HotSync tools to: /dev/pts/N`.
      3. `pilot-xfer -p /dev/pts/N -l` on the host.
      4. `palm_button name=cradle action=tap` (or tap HotSync app → Local).
      5. Record outcome (works / CMP handshake seen / nothing) with serial
         logging on (`log set Serial 2`... check exact category via
         `log list`).
- [ ] **4.2 If handshake stalls** — debug the UART/transport under load
      (FIFO overrun, RTS, `CycleSlowly` RX pump cadence,
      `EmUARTDragonball.cpp:630-647`); this is the only expected weak spot.
- [ ] **4.3 If timeouts trip on m500** — wall-pace the timer accumulator
      (raw clock) independently of the calibrated throttle clock — keeps
      ticks true during busy stretches.
- [ ] **4.4 Productize** — ReControl `info` (or new `serial`) reports the
      PTY slave path so WildPalms can script sync end-to-end; write
      `docs/hotsync.md` with the verified procedure.

**GATE 4:** `pilot-xfer -l` lists the device's databases; procedure
reproducible from docs/hotsync.md by a fresh session.

---

## Phase 5 — Declutter and ship 0.9.1

- [ ] Apply the remaining PROBABLY-SAFE deletions from the dead-code audit
      (`src/core/jpeg/` + the `DISABLE_JPEG_SUPPORT` fiction, `src/core/Gzip/`,
      UAE generator tools) — one commit each, build between.
- [ ] Small dedup: shared `ParseAddress` declaration in `ReControl.h`; the
      10-file `#undef daysInYear` preamble into one shim header; hoist
      `ReControlCmds_Profile.cpp`'s 7 duplicate stoppers.
- [ ] Fix `main.cpp:145` returning before `theApp.Shutdown()` (prefs not
      saved on normal exit).
- [ ] Update STATUS.md (only verified facts), release notes, tag 0.9.1,
      rebuild deb/AppImage/exe via existing packaging.

**GATE 5 (= v1.0 definition of done):** 30-minute autonomous agent session
(install/launch/crash/inspect/recover ×20) with zero restarts; HotSync
demonstrated; docs audit-clean (every CURRENT-STATE doc claim verifiable);
fresh clone builds and runs.

---

## What we are explicitly NOT doing

- No UAE core upgrade/replacement (two postmortems say why).
- No per-instruction cycle exactness beyond the calibration table — HotSync
  doesn't need it and the benchmark analysis showed the cost.
- No multi-client ReControl, no Windows-specific automation work until
  GATE 4.
- No new MCP features beyond Phase 3 until v1.0.
- ~~Landmine #7 root fix DEFERRED to post-v1.0~~ **DONE 2026-06-12 (deferral
  overridden by user decision)** — the per-site verdict cache landed on
  branch `landmine-7-root-fix` (plan:
  `docs/superpowers/plans/2026-06-11-landmine7-root-fix.md`). The Phase 3c
  diagnosis held: the residual freeze was the
  `ProbableCause → GetWhatHappened → AllowForBugs → FindFunctionName` per-
  access path. The fix analyzes each site (PC, size, meta-bit signature, r/w)
  once per arming and suppresses repeats, with generation + chunk-anchored
  invalidation and dynamic-context forgiveness never cached. See
  `docs/STATUS.md` landmine #7 (FIXED) for mechanism + evidence.
