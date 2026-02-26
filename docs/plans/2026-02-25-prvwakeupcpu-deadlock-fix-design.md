# Fix: PrvWakeUpCPU deadlock from pen/key events

**Date:** 2026-02-25

## Problem

`PostPenEvent` and `PostKeyEvent` call `PrvWakeUpCPU`, which blocks the
main Qt thread via `EmSessionStopper(kStopOnSysCall, 2000)` waiting for
the omni_thread CPU to reach a syscall boundary.  When the CPU is inside
`ExecuteSubroutine` (common after loading a `.psf` mid-syscall),
`kStopOnSysCall` cannot succeed because `ExecuteSubroutine` clears
`fSuspendBySysCall` and re-enters the CPU loop.  The 2-second timeout
doesn't help because Qt queues more mouse/pen events during the wait,
each triggering another 2-second block — permanently hanging the UI.

The same path is hit from ReControl: CPUWorkerThread calls
`PostPenEvent`, which bounces `PrvWakeUpCPU` to the main thread via
`QMetaObject::invokeMethod(Qt::QueuedConnection)`, blocking it there.

## Solution

Remove the `PrvWakeUpCPU` call from `PostPenEvent` and `PostKeyEvent`.

Events are already placed in thread-safe queues (`fPenQueue`,
`fKeyQueue`).  The CPU thread consumes them via the `SysEvGroupWait`
tailpatch in `EmPatchMgr.cpp`.  `PrvWakeUpCPU` / `EvtWakeup` only
serves to kick the CPU out of `EvtGetEvent` sleep faster.  Without
it, events are picked up at the next timer interrupt (~10 ms) or the
next natural `SysEvGroupWait` call — negligible latency.

`PrvWakeUpCPU` is NOT deleted; it remains in use by Gremlins, Hordes,
HostControl, and file import.

## Changes

**File:** `src/core/EmSession.cpp`

- `PostPenEvent`: remove `::PrvWakeUpCPU(kStr_EnterPen)` call
- `PostKeyEvent`: remove `::PrvWakeUpCPU(kStr_EnterPen)` call

No other files change.
