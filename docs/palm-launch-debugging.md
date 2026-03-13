# palm_launch Debugging Session — 2026-03-13

## Problem
`palm_launch` (ReControl `launch` command) returns OK but the app never switches.
PSF loading also broke (freeze on load) — fixed by reverting PrvWakeUpCPU re-addition.

## What We Tried (All Failed)

### 1. Direct SysUIAppSwitch (original approach, commit b9e58a0)
- Synchronous on main thread: `EmSessionStopper(kStopOnSysCall)` + `SysUIAppSwitch`
- SysUIAppSwitch returns 0 (success), ROM globals `nextUIAppDBID` correctly set
- But app never switches — SysUIAppSwitch only sets globals, doesn't post appStopEvent when called as subroutine

### 2. Worker thread SysUIAppSwitch + EvtWakeup
- Moved to CPUWorkerThread handler, EvtWakeup in response callback
- EvtWakeup returns 0 but SysEvGroupWait headpatch never fires afterward
- EvtWakeup sets `sendNullEvent` flag which bypasses SysEvGroupWait entirely

### 3. SetSwitchApp + EvtWakeup (PuppetString mechanism)
- Set `EmPatchState::SetSwitchApp(0, dbID)` for PuppetString to pick up
- Called EvtWakeup to cycle event loop
- PuppetString NEVER fires after EvtWakeup — confirmed with debug traces

### 4. SetSwitchApp + EvtWakeup in same stopper context
- Both calls inside single `EmSessionStopper(kStopOnSysCall)`
- Same result — PuppetString never fires

### 5. SysUIAppSwitch + EvtAddEventToQueue(appStopEvent) + EvtWakeup
- Directly posted appStopEvent (type=22) to Palm OS queue
- Called EvtWakeup to wake event loop
- Still no app switch

## Key Architecture Findings

### ExecuteSubroutine Context is Special
- `EmSession::ExecuteSubroutine` clears ALL suspend flags, runs CPU with `fNestLevel > 0`
- PuppetString checks `IsNested()` — **returns nil event and skips all event injection when nested**
- This means ROM trap calls made via subroutine (SysUIAppSwitch, EvtAddEventToQueue, EvtWakeup) run in a fundamentally different context

### PuppetString Gating Conditions
PuppetString (`EmPatchMgr.cpp:919`) requires ALL of:
1. Called from SysEvGroupWait or SysSemaphoreWait headpatch
2. `EmLowMem::GetEvtMgrIdle() == true`
3. `gSession->IsNested() == false` (if nested, forces nil event and returns)
4. Events in queue OR `EmPatchState::GetNextAppDbID() != 0`

### EvtWakeup Doesn't Trigger SysEvGroupWait Re-entry
- EvtWakeup (ROM trap 0xA12F) sets `sendNullEvent` flag in Palm OS memory
- This causes EvtGetEvent to return nil event WITHOUT calling SysEvGroupWait
- On NEXT EvtGetEvent call, SysEvGroupWait SHOULD be called (flag cleared)
- But in practice, PuppetString trace never fires after EvtWakeup
- Possible: the CPU never actually re-enters SysEvGroupWait after subroutine EvtWakeup

### How PuppetString App Switch Normally Works (Boot Time)
1. `PrvAutoload` calls `EmPatchState::SetSwitchApp(cardNo, dbID)`
2. Event loop starts, calls `EvtGetEvent` → `SysEvGroupWait`
3. SysEvGroupWait headpatch → PuppetString → finds `NextAppDbID != 0`
4. PuppetString calls `SwitchToApp` → `SysUIAppSwitch` (inside headpatch context, NOT nested subroutine)
5. PuppetString sets `clearTimeout = true` → SysEvGroupWait timeout modified to non-infinite
6. SysEvGroupWait returns quickly → EvtGetEvent returns → app processes switch

### The clearTimeout Mechanism is Critical
- PuppetString's SwitchToApp sets `clearTimeout = true`
- This modifies SysEvGroupWait's timeout parameter from infinite (0) to non-infinite (-1)
- Without this, SysEvGroupWait sleeps forever after PuppetString returns
- Direct SysUIAppSwitch calls DON'T set clearTimeout

### PSF Loading Freeze (Fixed)
- Caused by re-enabling `PrvWakeUpCPU` in `PostPenEvent`/`PostKeyEvent`
- Commit 7b51f89 deliberately removed it because it deadlocks when CPU is inside ExecuteSubroutine
- When loading PSF, CPU may be in nested context → EmSessionStopper blocks main thread forever
- Fix: keep PrvWakeUpCPU removed from PostPenEvent/PostKeyEvent (rely on natural ~10ms timer cycle)

## Resolution

The fix combines three mechanisms (a variant of Approach C — headpatch context):

1. **`SetSwitchApp(0, dbID)`** — schedules the switch via POSE-level globals (`fNextAppCardNo`, `fNextAppDbID`) that PuppetString checks
2. **`EvtWakeup()`** — ROM call from nested/subroutine context. Although PuppetString won't fire during the nested call (IsNested guard), EvtWakeup signals the kernel event group at the hardware level. This wakes the blocked `SysEvGroupWait` when the CPU resumes normal execution.
3. **New `SysHeadpatch::EvtGetEvent`** — catches pending app switches at EvtGetEvent entry, before EvtGetEvent decides whether to call SysEvGroupWait. This is a backup path for cases where EvtGetEvent returns cached events without reaching SysEvGroupWait.

The key insight: EvtWakeup's event group signal persists across the nested→normal context transition. Even though PuppetString is blocked during the subroutine call, the signal wakes SysEvGroupWait on the next natural event loop cycle, where PuppetString fires normally.

Tested with 4 consecutive app launches: Calculator → Memo Pad → Date Book → Calculator. All switched correctly.

### Files Changed
- `EmPatchModuleSys.cpp/.h`: New `EvtGetEvent` headpatch (replaces `RecordTrapNumber` for EvtGetEvent)
- `ReControl.cpp`: `CmdLaunch` rewritten — uses SetSwitchApp + EvtWakeup instead of direct SysUIAppSwitch
- `EmPalmOS.cpp`: `BreakOnSysCall` now allows AMX root task context (fixes kStopOnSysCall timeout when idle)
- `EmSession.cpp/.h`: `SleepInterruptible` (replaces usleep for instant kStopNow response), `BlockOnDialog` fReset check
- `EmCPU68K.cpp`: SleepInterruptible in stopped loops, proportional throttle yield
- `EmDlgQt.cpp`: Deferred dialog button click, `EmDlgQt_DismissIfPending`
- `EmWindowQt.cpp`: Wayland window sizing fix (setWindowFlags before resize)
- `EmBankDRAM.cpp`: Removed debug trace
- `main.cpp`: Version bump to 0.9.1
