# Finding: the deferred-error dialog subsystem is broken (2026-06-10)

**Status:** OPEN. Discovered while building the reproduction for Phase 1 task 1.1
(landmine #2, the suspend-counter leak). This is a **hard prerequisite** for
tasks 1.1 and 1.2 — both need a working `blocked_on_ui` dialog to exercise
`kStopOnCycle` while the CPU is blocked, and right now raising such a dialog
hangs and then crashes.

**Repro:** `tests/phase1/repro_dialog_subsystem.py` (fails today).
**Severity:** This is itself a Phase-1 "freeze class" — arguably the worst one:
*any* guest error / watchpoint / spy that raises a dialog hangs the CPU, and the
documented recovery (`reset`) crashes the process. It likely contributed to the
project being "abandoned mid-debug." The audit (STATUS.md) inferred landmine #2
from code but never ran the repro, so it never hit this.

## How to reproduce (60 seconds)

```
build/pose64 -psf m515.psf --port 6416        # QT_QPA_PLATFORM=offscreen or xcb
printf 'spy set 0x134\n' | socat -t2 - TCP:localhost:6416   # 0x134 = a low-mem
                                                            # global written ~100x/s
# within ~1s: `state` -> OK blocked_on_ui
printf 'dialog\n'        | socat -t2 - TCP:localhost:6416   # -> OK none  (never shows!)
printf 'reset\n'         | socat -t2 - TCP:localhost:6416   # -> process SIGSEGV / wedge
```

Verified identically under `QT_QPA_PLATFORM=offscreen` **and** `xcb` (real X11),
so it is not a headless-platform quirk.

## Two coupled defects

### (A) The dialog never shows during normal idle → permanent hang

The CPU thread raises a deferred error in `EmSession::ExecuteSpecial`
(`EmSession.cpp:1412`, `(*iter)->Do()`), which runs
`Errors::ReportErrStepSpy` → `Errors::DoDialog` → `EmDlg::DoCommonDialog` →
`EmSession::BlockOnDialog` (`EmSession.cpp:1616`). `BlockOnDialog`:

1. `gDocument->ScheduleDialog(fn, parms, result)` posts an `EmActionDialog`
   (`EmDocument.cpp:708`) referencing the **stack-local** `EditCommonDialogData`
   built in `DoCommonDialog` (`EmDlg.cpp:4706-4715`).
2. Sets `fState = kBlockedOnUI` and waits:
   `while (result == kDlgItemNone && !fStop && !fReset) fSharedCondition.wait();`

The main thread's `EmApplicationQt::HandleIdle` →
`EmApplication::HandleIdle` → `EmDocument::HandleIdle` → `DoAll()` is supposed to
run `EmActionDialog::Do()` and show the QMessageBox. Empirically it does **not**
happen during normal idle: `dialog` returns `OK none` for many seconds and the
CPU stays `blocked_on_ui`. (Root cause of (A) not yet pinned — candidates: the
`PrvIdleClipboard` early-return at `EmApplicationQt.cpp:157`, the
`inHandleIdle` reentrancy guard at `EmApplication.cpp:395`, or `DoAll` not being
reached while the CPU thread holds state. Needs a focused session.)

### (B) `reset` while blocked → use-after-free crash

`reset` (`RcCmd_Reset`) calls `EmDlgQt_DismissIfPending()` then
`gSession->ForceReset()`. `ForceReset` sets `fReset` and broadcasts
`fSharedCondition`, so `BlockOnDialog`'s wait loop exits on `!fReset`
(`EmSession.cpp:1637`) **with the dialog unanswered**. The CPU thread unwinds —
`BlockOnDialog` → `DoCommonDialog` → `DoDialog` — destroying the stack-local
`EditCommonDialogData` and the `result` it referenced.

But the `EmActionDialog` posted in step 1 is **still queued**. When the main
thread later runs it:

```
EmActionDialog::Do()  (EmDocument.cpp:132)
  -> EmDlg::RunDialog(fDlgFn, fDlgParms)            // fDlgParms == &dead stack data
  -> PrvHostCommonDialog (EmDlgQt.cpp:660)
  -> QString::fromUtf8(data.fMessage)               // fMessage dangles -> strlen(garbage)
  -> SIGSEGV
```

Backtrace (main thread):
```
#0 QtPrivate::lengthHelperPointer<char>(char const*)
#1 QByteArrayView::QByteArrayView<char const*>(char const* const&)
#2 PrvHostCommonDialog(...)            EmDlgQt.cpp:705
#3 EmDlg::HostRunDialog(void const*)
#4 EmDlg::RunDialog(...)
#5 EmActionDialog::Do()                EmDocument.cpp:136
#6 EmActionHandler::DoAll()
#7 EmDocument::HandleIdle()
#8 EmApplication::HandleIdle()
```
`EmActionDialog::Do` also writes `fDlgResult` (a reference to the dead stack
`result`) — a second UAF.

## Fix approach (for a focused next session — R6: strongest model, careful)

The lifetime contract is the bug: a queued cross-thread action references
CPU-stack data whose lifetime is only guaranteed while `BlockOnDialog` is
*still blocking*. `fReset`/`fStop` breaking the block violates that.

Two complementary fixes:
1. **(B) UAF:** when `BlockOnDialog` exits early (`fReset`/`fStop`,
   `result == kDlgItemNone`), the still-queued `EmActionDialog` must be
   neutralised before the CPU stack is torn down. Options: cancel/remove the
   pending action from `EmActionHandler` under `fSharedLock`; or move the dialog
   request to a heap object with a shared "live" flag that `EmActionDialog::Do`
   checks (and that owns a *copy* of the message so even a late run is safe).
   The opaque `const void* parms` makes a generic copy hard — prefer the
   cancel-on-early-exit route, guarded by the shared lock against the
   main-thread `Do()` race.
2. **(A) hang:** determine why `DoAll()` doesn't show the dialog during idle
   while `blocked_on_ui`; the dialog must actually appear so `dialog` /
   `dialog respond` work as documented (and so the CPU can be unblocked the
   normal way, not only via `reset`).

GATE for this fix: `tests/phase1/repro_dialog_subsystem.py` passes, AND a spy/
watch/error dialog can be queried with `dialog` and dismissed with
`dialog respond` without crashing — which then unblocks the 1.1 reproduction
(`spy set 0x134` → `blocked_on_ui` → `ui` (leak) → dismiss → `state` must be
`running`, not `suspended:ui`).

## Impact on the Phase 1 plan

- **1.1 (suspend-counter leak):** fix is code-verified (decrement on the
  failure path in `SuspendThread`; `ForceReset` clears `fSuspendByUIThread`) but
  **cannot be runtime-verified** until this dialog bug is fixed. The fix was
  written and reverted (kept out of the baseline per R1/R4) — re-apply and
  verify after the dialog subsystem works. The exact diff is in
  `docs/superpowers/plans/2026-06-10-phase1-kill-freeze-classes.md` Task 1.1.
- **1.2 (universal stop timeouts):** same dependency for the blocked-CPU repro.
- Recommend inserting a new prerequisite task **"1.0d — repair the deferred-error
  dialog subsystem"** ahead of 1.1/1.2 in the recovery plan.
