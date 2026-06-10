# Phase 2 Planning Handoff — "Make Input Delivery Honest"

**Created:** 2026-06-10 · **Repo state:** HEAD `460449e`, tagged
`phase-1-complete`, tree clean · **Author:** planning session (paused for
hardware-emulation research).

> **RESOLVED 2026-06-10 (same day, resuming session):** the hardware-emulation
> research (Q-B1–Q-B3) and all decidable open questions in §6 are settled —
> see **§10 Decisions** at the bottom. Q-MECH itself remains at the
> measure-first checkpoint by design. Implementation plan:
> `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md`.

> **Why this doc exists.** Phase 2 planning *started* but is *paused*: choosing
> the delivery mechanism (recovery-plan task 2.2) depends on hardware-emulation
> minutiae (how `SysEvGroupWait`/STOP/`EvtWakeup` actually behave) that need
> dedicated research first. This doc hands a fresh session everything learned
> so far — the **verified** current behavior, the candidate mechanisms, and the
> **open questions that gate Phase 2** — so the next session can do the research,
> make the decisions, and go straight to `writing-plans` without re-deriving any
> of it.
>
> **Read order for the resuming session:** `docs/STATUS.md` →
> `docs/recovery-plan-2026-06.md` Phase 2 section → this doc →
> `docs/architecture.md` (Event Delivery / STOP / PuppetString sections).

---

## 1. Where we are

- **Phase 1 complete, GATE 1 passed** (TSAN 13/13 ×3; ASAN 30-min soak clean).
  HEAD is tagged `phase-1-complete`. **That milestone tag already exists — no
  action needed.**
- **Phase 2 = "Make input delivery honest."** Outcome (from the recovery plan):
  `OK` from `tap`/`key`/`type` means *delivered* (or you get a truthful error),
  with **one** delivery mechanism left in the tree. Tasks 2.1–2.4 + GATE 2.
- **Phase 2 planning is paused here** pending the research in §6/§7 below.

## 2. Decisions already made this session (do not re-litigate)

| # | Decision | Choice |
|---|----------|--------|
| Q1 | How to resolve the 2.2 mechanism choice (A poll-always vs B targeted-wake) | **Measure first, decide at a data-backed checkpoint.** Build the 2.1 delivery test + an idle-CPU harness, characterize today's failure rate AND approach A's idle cost, *then* choose. Default lean stays **B+C**, data may override to **A+C**. |
| Q2 | The delivered-ACK (C) response contract for `tap`/`key`/`type` | **UNRESOLVED — open question (§5, Q-ACK).** Posed but deferred to research. |

Settled constraints (also not up for re-debate):
- Plan artifact goes in `docs/superpowers/plans/` via `superpowers:writing-plans`
  (project convention overrides the brainstorming default of `specs/`).
- **GATE 2 thresholds:** delivery test ≥ 99% over 200 taps **at 1x and at Max
  speed**; idle CPU% recorded in `STATUS.md`; **exactly one** wake mechanism
  greppable in `src/`.
- Process rules **R1–R6** bind every Phase 2 session (reproduce-first;
  replace-don't-stack; effects-not-responses; clean tree; docs in the same
  commit; no mega-sessions).
- Recovery-plan recommendation lean: **B + C**; **A is acceptable as an interim**
  if measured idle cost is negligible at 1x.

---

## 3. Ground truth — how input delivery ACTUALLY works today

Verified by **reading the live code at HEAD `460449e`** (not the docs — the
docs were partly wrong here; see §9). File:line are current.

**The path of a `tap`/`pen`/`key`/`type`:**

1. ReControl dispatches it as **`kCmdWorkerDirect`** → runs on **CPUWorkerThread**,
   fire-and-forget. (Args are now validated on the main thread first — task 1.8.)
2. Handler calls `EmSession::PostPenEvent` (`EmSession.cpp:1995`) /
   `PostKeyEvent` (`EmSession.cpp:1954`).
3. **Gate:** both call `PrvCanBotherCPU()` (`EmSession.cpp:2068`) first. It
   returns **false** when **Hordes / EventPlayback / Minimize** is on → the
   event is **silently dropped and the caller still got `OK`**. (This is the
   "silently dropped" half of landmine #3 and the target of **task 2.4**.)
4. If allowed, the event goes onto `fPenQueue`/`fKeyQueue` (thread-safe). **No
   wake is performed** — the comments at `EmSession.cpp:1962` and `:2019`
   explicitly say "No need to call `PrvWakeUpCPU` here" because the old wake
   blocked the main thread / deadlocked when the CPU was nested.
5. The event is picked up later by **`PuppetString`** (`EmPatchMgr.cpp:919`), the
   headpatch on `SysEvGroupWait`, on the **CPU thread**.

**What `PuppetString` does in interactive mode (the key finding):**
`EmPatchMgr.cpp:1024-1085`. When idle, not nested, not Replay, not Hordes:
- key event → `StubAppEnqueueKey(...)` and **falls through** (no `clearTimeout`,
  no `kSkipROM`).
- pen event → `StubAppEnqueuePt(...)` and **falls through** (no `clearTimeout`,
  no `kSkipROM`).
- `clearTimeout = true` is set **only** for Replay (`:980`), Hordes (`:1018`),
  and app-switch (`:1084`). **It is NOT set for plain interactive pen/key
  delivery.** So the real ROM `SysEvGroupWait` runs and is free to sleep with an
  infinite timeout.

This is the opposite of what `architecture.md` claimed ("we set
`clearTimeout = true` for interactive mode … the CPU doesn't sleep anymore").
That claim is **false at HEAD** — corrected in §9.

**`PrvWakeUpCPU`** (`EmSession.cpp:2092`) still exists (main-thread bounce +
`kStopOnSysCall` 2 s stopper + `EvtWakeup`) but is **dead**: a whole-tree grep
finds **no live callers** — only comments mention it. It is the old, removed
mechanism. → Safe to delete in **task 2.3** (confirm again before deleting).

## 4. The two real failure modes (what Phase 2 must fix)

1. **Guest asleep ⇒ undelivered.** If the app is idle in
   `SysEvGroupWait(evtWaitForever)` and has already executed `STOP`, posting a
   pen/key event does nothing to re-trigger the headpatch — `PuppetString` only
   fires on **trap entry**, not while the ROM spins in its internal wait loop.
   Timer interrupts wake the CPU each tick but return into the *same* sleeping
   wait loop (event group not signaled). The event sits in the queue until a
   *real* event (button, etc.) re-enters the trap. This is what the removed
   `PrvWakeUpCPU`/`EvtWakeup` used to paper over.
2. **Silently dropped ⇒ lying `OK`.** `PrvCanBotherCPU()` false (Hordes / Replay
   / Minimize) → event discarded, `OK` returned anyway. → task 2.4.

## 5. Candidate mechanisms (and the gap I found)

These map to recovery-plan task 2.2. **A and B are mutually exclusive** ways to
beat failure mode #1; **C is an orthogonal honesty layer** on top of either.

- **A — Poll-always** (== the deferred 2026-03-13 patch,
  `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`, minus
  its `fprintf` debug lines). In interactive mode, always `clearTimeout = true`
  and `kSkipROM`+`PrvForceNilEvent` right after enqueuing. The guest **never
  sleeps** while interactive, so a posted event is delivered on the very next
  `EvtGetEvent`. **Cost:** the CPU never enters STOP at idle → idle CPU rises.
  *The "sleep-until-interrupt idle" behavior is a headline feature
  (`STATUS.md` "What verifiably works") — the recovery plan says measure this
  cost, don't destroy it silently.* Lowest implementation risk; ~80% already
  written in the patch.

- **B — Targeted wake.** Let the guest sleep normally; on enqueue, arrange a
  one-shot wake so `SysEvGroupWait` re-evaluates. **GAP (found this session):**
  as literally written in the recovery plan ("schedule a one-shot clear-timeout
  on the next `SysEvGroupWait`"), B does nothing for an **already-asleep** guest
  — there is no "next" trap entry until the CPU is woken. The robust version of
  B needs a **CPU-thread-side wake** — the safe reincarnation of `PrvWakeUpCPU`,
  run **on the CPU thread** (where running ROM is legal) instead of the main
  thread (where the old one deadlocked). Whether and how that is feasible is the
  **core hardware-emulation research** below (§6). Higher risk: it touches the
  STOP loop and ROM-call ownership — the most dangerous area per the Do-Not-Do
  list.

- **C — Delivered-ACK** (honesty). After enqueue, the worker blocks (≤ 2 s) on a
  **delivery counter that `PuppetString` increments** when it actually injects
  the event, so the protocol response is truthful (`OK delivered` /
  `ERR pending`). Independent of A vs B. This is what literally makes "`OK`
  means delivered" true.

---

## 6. OPEN QUESTIONS GATING PHASE 2

These are the blockers. The **mechanism** ones (Q-B*) are the
hardware-emulation research the resuming owner wanted to do before committing.

### Q-MECH — A vs B (strategy decided = measure-first; the *choice* is open)

Resolved **only** after (a) the 2.1 failure-rate data, (b) the idle-CPU cost of
A, and (c) the B-feasibility answers below. If B is infeasible or too invasive,
or A's measured idle cost is negligible at 1x, the answer collapses to **A+C**.

The B-feasibility sub-questions (the "minutia of hardware emulation"):

- **Q-B1 — Timeout consumption.** How does the ROM's `SysEvGroupWait` actually
  consume the timeout the headpatch sets, and can a one-shot "clear timeout"
  reliably land **before** the guest commits to `STOP`? `PuppetString` runs only
  on trap **entry**; once the ROM is inside its wait loop, the headpatch won't
  fire again. (Files: `EmPatchModuleSys.cpp:1903` calls
  `PuppetString`; `EmCPU68K.cpp` STOP handling.)

- **Q-B2 — Waking an already-asleep guest, safely, on the CPU thread.**
  Constraints: you **cannot** `ExecuteSubroutine` while the CPU is stopped
  (`EmAssert(oldRegs.stopped == 0)`; architecture Do-Not-Do #11), and the old
  `PrvWakeUpCPU` ran `EvtWakeup` off the **main** thread → deadlock. Candidate:
  have `ExecuteStoppedLoop` (`EmCPU68K.cpp:761`, CPU thread) detect a pending
  pen/key flag and break STOP. **Does breaking STOP actually make
  `SysEvGroupWait` return**, or does it just re-enter STOP because the event
  group was never signaled? What's the minimal safe action on the CPU thread
  that causes the trap to be re-entered (so the headpatch can deliver)?

- **Q-B3 — `EvtWakeup` semantics.** `EvtWakeup` (`ROMStubs.cpp:1178`,
  `sysTrapEvtWakeup`) sets `sendNullEvent` (`SysEvtPrv.h:162`) so the next
  `SysEvGroupWait` returns a nilEvent immediately. **Can it be invoked from the
  CPU thread at a safe (non-stopped, non-nested) point** — e.g. defer the actual
  STOP by one tick when an event is pending, and call `EvtWakeup` there? That
  would be the legal, on-CPU-thread reincarnation B needs.

- **Q-B4 — How often does a real idle guest truly sleep?** If apps mostly poll
  with a finite timeout (cursor blink, etc.) rather than `evtWaitForever`, the
  "already-asleep" gap is rare and a wake-before-sleep variant of B suffices.
  **Partly answered by the 2.1 idle measurement** — characterize whether the
  launcher / an app at rest enters STOP at all.

### Q-ACK — The delivered-ACK contract (was Q2; UNRESOLVED)

Changing what `tap`/`key`/`type` return touches **every** caller: the Phase 1
repros (`tests/phase1/*`), `test_recontrol_stress.py`, the **28 MCP tools**, and
the `pose64-tester` agent. Choose:

- **(a) Change the default contract** — they block ≤ 2 s on the delivery counter
  and return `OK delivered` / `ERR pending` / `ERR busy`. This is the literal
  Phase 2 outcome. **Breaking:** update all callers + docs (R5); adjust existing
  tests to tolerate the new responses. *Mitigation:* build the 2.1 delivery test
  to assert on the **screen-hash effect**, not the response string, so it stays
  valid regardless of this choice.
- **(b) Opt-in await flag** — keep fire-and-forget `OK` as default; add an
  explicit await (`tap … await` or a new verb) for the blocking ACK.
  Non-breaking, but the default `OK` still over-promises.
- **(c) Defer** — decide (a) vs (b) at the same data-backed checkpoint as
  Q-MECH, recorded in `architecture.md`.

### Other open questions

- **Q-TEST (task 2.1 design).** The delivery test is the **referee** for
  everything. Decide: which known target to tap (Launcher icon via `tap-id` is
  the obvious choice), poll `screen-hash` vs `ui` (or both) within ≤ 2 s, the
  pass tolerance, and how to run the same script at **1x and Max** speed for the
  200-tap GATE. Must be **effect-based** (R3) and live in `tests/phase2/`.
  Build it FIRST and run against the current baseline to characterize today's
  failure rate.
- **Q-IDLE (idle-CPU methodology).** GATE 2 requires recording idle CPU% in
  `STATUS.md`. Define: device/ROM, what "idle" means (booted to launcher, no
  input, N s), host measurement (`pidstat`/`top`/`perf`), at 1x and Max. Measure
  baseline, then **apply patch A, measure, revert** — this number is the whole
  basis for the A-vs-B call.
- **Q-DROP (task 2.4 plumbing).** The `PrvCanBotherCPU` drop happens **deep
  inside** `PostPenEvent`/`PostKeyEvent` on the worker thread. Surfacing
  `ERR busy: gremlin running` instead of `OK` needs a **return channel** from
  the post path back to the worker handler. The task-1.8 main-thread validation
  (`RcValidate_*`) is a hook, but Hordes/Replay/Minimize state is only known at
  post time — decide where the check lives and how the handler returns the error.
- **Q-SYNC (delivery counter, only if C adopted).** Design the cross-thread
  primitive: `PuppetString` (CPU thread) increments a thread-safe counter when
  it injects; the worker waits (≤ 2 s) on a condition. Define what "delivered"
  means precisely — `StubAppEnqueuePt` called, or the app actually dequeued it?
- **Q-CLEAN (task 2.3, R2).** Delete dead `PrvWakeUpCPU` + its declaration
  (`EmSession.cpp:60-61, 2089-2135`), and remove the stale "bridge thread"
  comments at `EmSession.cpp:1325` and `EmWindow.cpp:489`. (The
  `EmApplicationQt.h:7` / `EmWindowQt.h:9` / `EmWindowQt.cpp:9` "NO bridge
  thread" notes are **correct** — keep them.) Do this in the **same commit** as
  the winning mechanism so only one wake path is ever greppable (GATE 2).
- **Q-SPEED ("Max" mechanics).** Confirm how GATE 2 sets Max speed
  (`fEmulationSpeed` atomic) and that the chosen mechanism still hits ≥ 99%
  delivery unthrottled.
- **Q-DEV (device/ROM choice).** Idle CPU at "1x" is calibration-sensitive
  (m500 calibration corrects the throttle, not the timer — see STATUS HotSync
  note). Decide whether Phase 2 measures on the default dev device (m515) or an
  uncalibrated one, and record the caveat.

---

## 7. Recommended first moves for the resuming session

1. Re-read STATUS.md + recovery-plan Phase 2 + this doc. (`phase-1-complete` is
   already tagged — skip that.)
2. **Build the 2.1 delivery test** (effect-based, `tests/phase2/`); run against
   the baseline; record today's failure rate. [R1/R3]
3. **Build the idle-CPU harness** (Q-IDLE); record baseline idle; then apply
   patch A, measure, **revert**; record A's idle cost.
4. **Do the hardware-emulation research** (Q-B1…Q-B4) to decide whether robust B
   is feasible and how invasive it is.
5. **Make the Q-MECH + Q-ACK decisions** at the checkpoint; **record them in
   `architecture.md`** (and finish correcting the Event-Delivery/STOP sections —
   see §9).
6. `superpowers:writing-plans` → the Phase 2 implementation plan in
   `docs/superpowers/plans/`; then execute reproduce-first, deleting the losers
   (Q-CLEAN) in the same commit as the winner.

## 8. Key file map for Phase 2

| Concern | Location |
|---|---|
| PuppetString delivery logic | `src/core/Patches/EmPatchMgr.cpp:919-1094` |
| Headpatch entry that calls PuppetString | `src/core/Patches/EmPatchModuleSys.cpp:1903` |
| Post path + `PrvCanBotherCPU` + dead `PrvWakeUpCPU` | `src/core/EmSession.cpp:1954-2135` |
| STOP loop (CPU thread) | `src/core/Hardware/EmCPU68K.cpp:761` (`ExecuteStoppedLoop`) |
| `EvtWakeup` stub / `sendNullEvent` | `src/core/ROMStubs.cpp:1178`, `…/SysEvtPrv.h:162` |
| Deferred approach-A patch | `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch` |
| Recovery-plan Phase 2 spec | `docs/recovery-plan-2026-06.md` §"Phase 2" |

## 9. Doc-truth correction (logged here, applied in the same session)

`docs/architecture.md` claimed in two places that `clearTimeout = true` is
already set for interactive mode so "the CPU doesn't sleep anymore." That is
**false at HEAD `460449e`** (verified by reading `EmPatchMgr.cpp:1024-1085` —
interactive pen/key delivery sets neither `clearTimeout` nor `kSkipROM`). Those
sections were corrected this session to describe the actual behavior and to flag
that idle runtime behavior is **unmeasured pending Phase 2**. This is exactly the
"trust the code, not the doc" failure class the project died of — if you find any
other doc asserting interactive delivery is already poll-always, treat it as
aspirational until re-verified.

---

## 10. DECISIONS (2026-06-10, resuming session)

All §6 questions resolved except Q-MECH, which stays at the measure-first
checkpoint per the settled strategy. Research was done by reading the live
code at HEAD `460449e`; file:line refs verified.

### Q-B1 — answered: NO, a one-shot clear-timeout cannot reach an asleep guest

The headpatch (`EmPatchModuleSys.cpp:1883`) reads and rewrites the guest's
`timeout` parameter **only at trap entry** (`:1910-1913`). Once the kernel has
consumed the timeout and blocked the task, there is no later point to apply
it. The recovery plan's literal wording of B ("clear timeout on next
SysEvGroupWait") is confirmed insufficient for an already-asleep guest.

### Q-B2 — answered: NO, breaking STOP alone does not deliver

`ExecuteStoppedLoop` (`EmCPU68K.cpp:761-998`) exits STOP only via
`ProcessInterrupt` (`:983-988`) or a host-side session break. Clearing
`SPCFLAG_STOP` without signaling the event group resumes the kernel idle
loop, which finds no ready task and re-enters STOP — timer ticks already do
this every ~10 ms without delivering. The event group must be signaled by
running ROM code (`EvtWakeup`); direct memory writes are Do-Not-Do #14.

### Q-B3 — answered: YES — the STOP-exit `EvtWakeup` hook (robust B exists)

The one legal wake point is **inside `ExecuteStoppedLoop`, immediately after
`ProcessInterrupt` clears `regs.stopped`** on a natural timer-tick wake:

- CPU thread (Do-Not-Do #1 ✓); never nested there (`EmCPU68K.cpp:766` ✓).
- `regs.stopped == 0` → `EmSubroutine.cpp:2503` / `ATraps.cpp:102` asserts
  hold (Do-Not-Do #11 ✓).
- `ExecuteSubroutine`'s CPU-thread assert (`fState == kRunning`,
  `EmSession.cpp:1280`) holds — the *session* runs while the guest CPU stops.
- Calling `EvtWakeup` at interrupt entry (exception frame pushed, ISR not yet
  run) is equivalent to the ISR calling it first — the documented Palm OS
  pattern; the stub's own comment (`ROMStubs.cpp:1166-1175`) names exactly
  this use case. Reentrancy (#13) ✓: a guest in STOP is in the kernel idle
  loop, not inside `EvtWakeup`.

**Design (~30–50 lines):** on STOP-exit via real interrupt, if
`HasPenEvent() || HasKeyEvent()` and not `EmHAL::GetAsleep()`, call
`EvtWakeup` once via `ExecuteSubroutine`. `EvtWakeup` sets `sendNullEvent` +
signals the event group → `SysEvGroupWait` returns → nilEvent → app calls
`EvtGetEvent` again → trap re-entry → existing PuppetString fall-through
delivers. Added latency ≤ 1 timer tick (~10 ms at 1x). Tick rate naturally
debounces repeat wakes. This is the safe reincarnation of `PrvWakeUpCPU` —
right thread, no blocking stopper.

### Q-B4 — deferred to data (by design)

Answered empirically by the 2.1 test's idle-first mode + idle-CPU harness.
Note the tension to resolve: taps demonstrably work today, so either idle
apps poll with finite timeouts (asleep gap rare) or something else re-enters
the trap.

### Q-MECH — still at the checkpoint, with one added criterion

Research adds a **guest-visible-behavior criterion** beyond idle CPU%: A
floods apps with nilEvents (`SysEvGroupWait` never blocks), distorting
nilEvent-cadence timing, auto-off, battery sim. B preserves guest timing at
the cost of ≤1 tick latency. **Decision rule:** if B's focused experiment
passes (tap delivered to a verified-asleep guest, TSAN-clean), pick **B+C**
even if A's idle cost measures small; **A+C** is the fallback if the
experiment surfaces kernel-state surprises.

### Q-ACK — DECIDED: (a) change the default contract

`tap`/`pen`/`key`/`type` block ≤2 s on delivery and return truthfully:
**`OK delivered`** (keeps `startswith("OK")` callers passing — every checked
caller except `repro_1_8_argval.py:54-60` exact-match dict, a 7-line update)
/ `ERR pending: queued, not delivered within 2000ms` (event remains queued —
a retry can double-deliver; document it). Mechanically this is a **dispatch
category change**: `kCmdWorkerDirect` → `QueueWork` discards the handler's
return and hardcodes `OK\n` (`ReControl.cpp:202-223`); move these commands to
a result-bearing category (`QueueWorkResult` pattern) and the handlers'
existing return strings become the response. MCP proxy relays text verbatim —
no proxy change. **Scope:** PuppetString-delivered input only; `button` keeps
its contract (hardware-ISR path, genuinely reliable); `tap-id` returns
`OK delivered <x> <y>`.

### Q-DROP — DECIDED: status-returning post functions

`EmSession::PostPenEvent`/`PostKeyEvent` (`EmSession.cpp:1954, 1995`) change
from `void` to a status enum: `kPosted` / `kDroppedGremlins` /
`kDroppedReplay` / `kDroppedMinimize` / `kDroppedDuplicate`. Check stays at
post time (only place Hordes/Replay/Minimize state is known); the Q-ACK
category change is the return channel; GUI callers ignore the return. The
pen-down dedup at `EmSession.cpp:2005` is a second silent drop — gets
`kDroppedDuplicate` → `ERR duplicate: pen already down`.

### Q-SYNC — DECIDED: delivery = OS-queue handoff; seq counters + dedicated lock

"Delivered" = PuppetString's enqueue stub returned (event entered the Palm OS
event queue — the same guarantee real hardware gives). Primitive: two
monotonic `uint64` counters in `EmSession` (`fInputPostedSeq` on successful
post, `fInputDeliveredSeq` incremented by PuppetString after
`StubAppEnqueueKey`/`Pt` returns) + a **dedicated** `omni_mutex`/
`omni_condition` (NOT `fSharedLock` — lock-ordering risk). Worker computes
its target seq from its own posts and waits with an absolute deadline
computed once before the loop (Do-Not-Do #3). A `tap` waits for both pen-down
and pen-up.

### Q-TEST — DECIDED: Find-dialog toggle, two modes, effect-based

`tests/phase2/test_delivery.py`, importing `tests/phase1/_harness.py` (don't
fork). Referee = `screen-hash` (R3): hash → raw **`tap`** (the WorkerDirect
path under test — NOT `tap-id`, whose stopper changes timing) → poll hash
every 50–100 ms ≤2 s → effect = changed. Target: **Find** silk icon opens the
Find dialog (hash change); tap **Cancel** (coordinate via `ui` once) closes
it. Two required modes: **rapid-fire** (awake path) and **idle-first** (~1 s
pre-tap idle so the guest can reach STOP — exercises failure mode #1, answers
Q-B4). Record per-tap delivery latency, not just pass/fail. Run at 1x and Max
(via `speed` command); GATE 2 ≥99% / 200 taps per speed. Build FIRST, run
against baseline, record today's failure rate per mode in STATUS.md.

### Q-IDLE — DECIDED: pidstat over self-launched offscreen instance

`tests/phase2/measure_idle_cpu.py`: boot `m515.psf` to launcher, no input,
10 s settle, `pidstat -p <pid> 1 60` → 60-sample mean; run 3×, record median;
at 1x and Max. Then baseline at HEAD → apply the 2026-03-13 patch (minus
`fprintf`s) on a scratch branch → measure → **revert**. Caveats to record:
offscreen platform (no real paint cost); **Max idle already pegs a core at
baseline** (STOP-loop sleep is `speed > 0`-gated, `EmCPU68K.cpp:926-942`) —
A's idle cost is only meaningful at 1x.

### Q-SPEED — DECIDED: add a `speed` ReControl command

No speed surface exists today — `fEmulationSpeed` is GUI-menu-only
(`EmApplication.cpp:1055`, percent: `100`=1x, `0`=Max). GATE 2 cannot run
without one. Add `speed [<pct>|max]`, `kCmdImmediate` (one atomic store +
preference write mirroring `DoSetSpeed`); bare `speed` queries. Document in
`recontrol-protocol.md` same commit (R5). **Not** a new MCP tool (28-tool
surface frozen until Phase 3; tests use TCP). Rejected alternative:
pre-writing the preferences file (brittle; can't switch mid-gate).

### Q-DEV — DECIDED: m515

Harness default (`_harness.py:41`); calibration exists only for m500, so m515
ticks are wall-true at 1x — what idle-CPU numbers need. Record that numbers
are m515-specific. One device for the whole gate.

### Q-CLEAN — CONFIRMED, plus one addition

Re-verified at HEAD: `PrvWakeUpCPU` has zero live callers (declaration
`EmSession.cpp:61`, definition `:2092`, its own recursive bounce `:2123`,
comments only). Delete in the same commit as the winning mechanism, plus
stale "bridge thread" comments (`EmSession.cpp:1325`, `EmWindow.cpp:489`)
**and** the stale `PrvWakeUpCPU` reference in `CPUWorkerThread.h:19` (not on
the original list). Reword Do-Not-Do #8 to past tense, keep the lesson.
Either winner satisfies GATE 2's grep test (B: the hook is the one wake
mechanism; A: zero artificial wake mechanisms).
