# Phase 1 — session handoff for the plan designer (2026-06-10)

**Purpose:** Hand off to whoever revises the recovery plan. Summarizes what this
session executed, the plan it followed, and — the main point — a precise,
previously-unknown bug that blocks the two headline Phase 1 fixes and warrants a
replan. Deep technical analysis of the bug:
[`2026-06-10-dialog-subsystem.md`](2026-06-10-dialog-subsystem.md).

---

## 1. What this session did (all committed + pushed to `master`)

| Commit | Task | Status |
|---|---|---|
| `3761726` | Phase 1 implementation plan (writing-plans) → `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` | done |
| `5f5c443` | **1.8** WorkerDirect arg validation on the main thread | verified ✅ |
| `08d8690` | **1.3** MCP proxy read timeout + no double-execute | verified ✅ |
| `b5363ea` | **Discovery**: dialog subsystem broken (repro + analysis + STATUS landmine #9) | reproduced ✅ |

Infrastructure added: `tests/phase1/` — an offscreen self-launching reproduction
harness (`_harness.py`) plus per-task repros. Every fix was **reproduce-first**
(repro fails before the fix, passes after) per recovery-plan rule R1, and
assertions are **effect-based** per R3.

Both shipped fixes are independent of the blocker below:
- **1.8** — `tap banana`, `tap 5 banana`, `key abc`, `button banana tap`, etc. now
  return `ERR usage` immediately (validated on the main thread) instead of a
  swallowed `OK`. (`CommandEntry::validate` + `RcValidate_*`.)
- **1.3** — the proxy now sets `SO_RCVTIMEO` (a wedged server → `ERR timeout`
  instead of hanging every later MCP call) and never reconnects-and-resends a
  non-idempotent command after a dropped response (no more double-`install`).

## 2. The plan being followed

- **Strategic roadmap:** `docs/recovery-plan-2026-06.md` — Phase 1 ("Kill the
  freeze classes") = 8 ranked tasks 1.1–1.8, each *failing repro → fix → effect
  test → commit*, gated by GATE 1 (30-min ASAN soak + TSAN clean).
- **This session's detailed plan:**
  `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` (writing-plans;
  real current line numbers; the two dangerous tasks 1.5/1.6 carry a
  reproduce-first STOP rule).
- **Binding rules:** R1 reproduce-first, R2 replace-don't-stack, R3
  effects-not-responses, R4 clean tree per session, R5 docs-in-same-commit, R6
  strongest model / one phase-task per sitting for threading.
- Execution order started with **1.1** (the suspend-counter leak, landmine #2 —
  the project's signature "permanently locked emulation" bug). Building its
  reproduction is what surfaced the blocker.

## 3. The precise bug (blocks 1.1 and 1.2)

**Symptom:** raising *any* deferred-error dialog (a `spy`/`watch` hit or a guest
memory-access/illegal-instruction error) is broken two ways — it hangs, and then
`reset` crashes the process.

**Reproduction** (`tests/phase1/repro_dialog_subsystem.py`, 60 s, verified on
both `QT_QPA_PLATFORM=offscreen` and `xcb`):
```
spy set 0x134     # 0x134 is a low-mem global written ~100x/s; raises a step-spy error
state             # -> OK blocked_on_ui      (CPU now blocked)
dialog            # -> OK none               (BUG A: the dialog never actually shows)
reset             # -> process SIGSEGV       (BUG B: use-after-free)
```

**Mechanism.** A deferred error is processed on the **CPU thread** in
`EmSession::ExecuteSpecial` (`EmSession.cpp:1412`), which calls
`Errors::ReportErrStepSpy → DoDialog → EmDlg::DoCommonDialog →
EmSession::BlockOnDialog` (`EmSession.cpp:1616`). `BlockOnDialog`:
1. posts an `EmActionDialog` to the **main thread** (`EmDocument::ScheduleDialog`,
   `EmDocument.cpp:708`) that references the **stack-local** `EditCommonDialogData`
   built in `DoCommonDialog` (`EmDlg.cpp:4706-4715`);
2. sets `fState = kBlockedOnUI` and waits:
   `while (result == kDlgItemNone && !fStop && !fReset) fSharedCondition.wait();`
   (`EmSession.cpp:1637`).

- **Bug A (hang):** during normal idle the main thread
  (`HandleIdle → DoAll → EmActionDialog::Do`) does **not** show the dialog — empirically
  `dialog` returns `none` for many seconds and the CPU stays `blocked_on_ui`. Root
  cause not yet pinned (candidates: the `PrvIdleClipboard` early-return at
  `EmApplicationQt.cpp:157`, the `inHandleIdle` reentrancy guard at
  `EmApplication.cpp:395`, or `DoAll` simply not reached while blocked). This alone
  means **any guest error permanently hangs the CPU.**
- **Bug B (UAF crash):** `reset` (`ForceReset`) sets `fReset`, so the
  `BlockOnDialog` loop exits **with the dialog unanswered**. The CPU thread
  unwinds and frees the stack-local dialog data — but the `EmActionDialog` is
  **still queued**. The main thread then runs it:
  `EmActionDialog::Do → EmDlg::RunDialog(fDlgParms) → PrvHostCommonDialog →
  QString::fromUtf8(data.fMessage)` with `fMessage` dangling → `strlen` over freed
  memory → **SIGSEGV**. (`fDlgResult`, a reference to the dead stack `result`, is a
  second UAF.) Backtrace and full analysis in the linked dialog-subsystem doc.

This is a genuine cross-thread lifetime bug: a queued main-thread action
references CPU-stack data whose lifetime is only valid *while `BlockOnDialog` is
still blocking*, and `fReset`/`fStop` breaking the block violates that contract.

## 4. How it affects the plan

- **1.1 (suspend-counter leak) and 1.2 (universal stop timeouts) cannot be
  runtime-verified** as written: both require a live `blocked_on_ui` dialog to
  exercise `kStopOnCycle`/`kStopNow` while the CPU is blocked, and reaching that
  state currently hangs and then crashes. The recovery plan's repro recipe for
  1.1 ("raise an error dialog … then dismiss") implicitly assumed dialogs work —
  they don't. The audit never executed it, so it never hit this.
- The **1.1 fix is code-correct** (in `SuspendThread`, decrement
  `fSuspendByUIThread` on the failure path that currently leaks it; have
  `ForceReset` clear that counter) but was **written and reverted** this session —
  an unverified threading change must not become the baseline (R1/R4). The exact
  diff is in the plan doc, Task 1.1.
- This dialog bug is **itself a Phase-1 "freeze class"** — arguably the worst one
  (every guest error → hang/crash) — yet it is **not** in the current 8-task list.

**Recommended replan:** insert a new prerequisite task **"1.0d — repair the
deferred-error dialog subsystem"** ahead of 1.1/1.2. It has two parts (Bug A
hang, Bug B UAF); both need careful cross-thread work (R6: focused sitting,
strongest model). Its gate: `repro_dialog_subsystem.py` passes **and** a
spy/watch/error dialog can be queried with `dialog` and dismissed with
`dialog respond` without crashing — which then unblocks the 1.1/1.2 repros.

**Unaffected, still executable now:** 1.5 (single ROM-call owner), 1.6
(PaintScreen read), 1.7 (`fLastPenEvent` race) — independent of dialogs, but each
needs a TSAN build to verify its race. 1.4 (bounded worker shutdown) likely needs
the same wedge mechanism as 1.1/1.2 to reproduce, so it pairs with the dialog work.

## 5. Pointers

- Deep dialog analysis + fix options: `docs/superpowers/findings/2026-06-10-dialog-subsystem.md`
- STATUS.md landmine #9 (the new entry)
- Phase 1 plan (incl. the reverted 1.1 diff): `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md`
- Repros + harness: `tests/phase1/` (`repro_dialog_subsystem.py`, `repro_1_8_argval.py`, `repro_1_3_proxy.py`, `_harness.py`)
