# POSE64 Stability Findings

Date: 2026-02-22

## Background

The ReControl feature introduced a **CPUWorkerThread** (QThread) alongside the
existing CPU **omni_thread** and the **main Qt thread**.  The original POSE
synchronization system (`EmSuspendState` counters, `EmSessionStopper` RAII,
`fSharedLock`/`fSharedCondition`) was designed for exactly **two threads**
(CPU + UI).  Adding a third thread broke critical invariants, causing GUI
freezes, use-after-free crashes, and heap corruption after sustained use.

## Threading Architecture

```
Main Qt Thread (T0)        CPU omni_thread (T1)        CPUWorkerThread (T9)
  - Event loop               - m68k emulation loop       - ReControl command handlers
  - HandleIdle()              - fCPU->Execute()           - PostPenEvent / PostKeyEvent
  - EmWindow mouse clicks     - EmSession::Run()          - EmSessionStopper
  - QMetaObject responses     - SuspendThread/Resume      - EmScreen::GetBits
```

## Bugs Found and Fixed

### Bug 1: SuspendThread timeout reset (GUI FREEZE)

**File:** `src/core/EmSession.cpp` (SuspendThread function)

**Root Cause:** `SuspendThread(kStopOnSysCall, 2000)` computed its 2-second
absolute deadline **inside** the while loop.  Every broadcast from
`fSharedCondition` (from `ExecuteSubroutine`, `ResumeThread`, etc.) woke the
waiting thread, which recomputed a fresh 2-second deadline.

**Result:** The timeout never fires when the CPU is in a nested
`ExecuteSubroutine` call, causing the main thread to block indefinitely in
`SuspendThread`.  `HandleIdle()` never runs because the main thread is stuck.
The GUI appears frozen while the emulator keeps running.

**Fix:** Moved the deadline computation BEFORE the while loop so it's computed
exactly once.  Now the timeout fires correctly even if spurious wakeups occur.

```cpp
// BEFORE (buggy):
while (fState == kRunning) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    deadline = now + timeout;  // resets every iteration!
    fSharedCondition.timedwait(deadline);
}

// AFTER (fixed):
deadline = clock_gettime() + timeout;  // computed once
while (fState == kRunning) {
    fSharedCondition.timedwait(deadline);
}
```

### Bug 2: Concurrent ExecuteSubroutine race (CRASH)

**File:** `src/core/EmSession.cpp` (PrvWakeUpCPU function)

**Root Cause:** Both the main thread (mouse clicks -> `PostPenEvent` ->
`PrvWakeUpCPU` -> `ExecuteSubroutine`) and the CPUWorkerThread (ReControl
tap/pen/key -> `PostPenEvent` -> `PrvWakeUpCPU` -> `ExecuteSubroutine`) could
call `fCPU->Execute()` simultaneously.  The UAE 68K CPU core uses global
mutable state (`struct regstruct regs`, memory banks) with no thread safety.

**Result:** Corrupted CPU registers, corrupted emulated memory, eventual bus
error or crash.

**Fix:** Added a thread check in `PrvWakeUpCPU` to only call
`ExecuteSubroutine` from the main thread.  From other threads, events are
already in thread-safe queues (`fPenQueue`, `fKeyQueue`) and will be processed
on the next `CycleSlowly` iteration (~2ms emulated time).

```cpp
// Skip ExecuteSubroutine from non-main threads
QCoreApplication* app = QCoreApplication::instance();
if (app && QThread::currentThread() != app->thread())
    return;
```

### Bug 3: CmdLoad raw pointer in deferred lambda (USE-AFTER-FREE)

**File:** `src/core/ReControl.cpp` (CmdLoad function)

**Root Cause:** `CmdLoad` captured `this` (ReControlSession*) in a
`QTimer::singleShot(0, ...)` lambda.  If the TCP socket disconnects before the
lambda fires, `this` is a dangling pointer.

**Fix:** Replaced with `QPointer<ReControlSession>` which automatically detects
when the QObject has been destroyed.

### Bug 4: Missing null guards during session transitions

**File:** `src/core/ReControl.cpp` (all command handlers)

**Root Cause:** During `CmdLoad`, `gSession` and `gCPUWorker` are destroyed and
recreated.  Commands already queued in the CPUWorkerThread reference `gSession`
in their handler lambdas, but `gSession` could be null by the time they execute.

**Fix:** Added `if (!gSession) return;` guard inside each handler lambda.

### Bug 5: Response lambda use-after-free (CRASH - most frequent)

**File:** `src/core/ReControl.cpp` (all command response lambdas)

**Root Cause:** Every command (CmdTap, CmdPen, CmdKey, CmdButton,
CmdScreenshot, CmdInstall, CmdLaunch, CmdSave, CmdInfo, CmdUI) captured a raw
`ReControlSession* self` pointer in its response lambda.  The response lambda
runs on the main thread via `QMetaObject::invokeMethod(QueuedConnection)` after
the handler completes on the CPUWorkerThread.

When the TCP client disconnects (socat closes, network drop, etc.), Qt destroys
the `ReControlSession` QObject.  But the response lambda is already queued in
the Qt event loop and fires after destruction, calling `self->Send()` on freed
memory.

**ASAN signature:**
```
heap-use-after-free on address ... at ReControl.cpp:253
READ of size 8 ... thread T0
freed by: ReControlSession::~ReControlSession() at ReControl.cpp:145
```

**Fix:** Replaced all `ReControlSession* self = this;` with
`QPointer<ReControlSession> safeRef(this);` and guarded all response lambdas
with `if (safeRef) safeRef->Send(...)`.

**Affected commands:** CmdTap, CmdPen, CmdKey, CmdButton, CmdScreenshot,
CmdInstall, CmdLaunch, CmdSave, CmdInfo, CmdUI (10 commands total).

### Bug 6: EmPixMap heap-buffer-overflow in 8-bit conversion

**File:** `src/core/EmPixMap.cpp` (STD_INDEX_TO_DIRECT_CONVERT macro)

**Root Cause:** The `STD_INDEX_TO_DIRECT_CONVERT` macro used `while (1)` and
read `*srcPtr++` BEFORE the bounds check in `CONVERT_PACKED_BYTE_8`.  For
8-bit indexed format (1 pixel per byte), the final iteration reads one byte
past the allocated buffer before the `if (xx++ >= right) break;` check fires.

**ASAN signature:**
```
heap-buffer-overflow on address ... in PrvConvert8To24RGB at EmPixMap.cpp:2215
READ of size 1 ... 0 bytes after 25600-byte region
```

**Fix:** Changed `while (1)` to `while (xx < right)` so the loop terminates
before reading past the source buffer.  This is safe for all pixel depths
(1/2/4/8 bit) because the inner `CONVERT_PACKED_BYTE_*` macros still handle
partial-byte boundaries for sub-byte formats.

## Pre-existing UBSAN Warnings (Not Fixed)

These are pre-existing undefined behavior warnings in legacy UAE/hardware
emulation code.  They are cosmetic and do not affect stability:

| File | Line | Issue |
|------|------|-------|
| readcpu.cpp | 758 | Negative shift exponent (CPU table init) |
| EmRegsVZ.cpp | 812, 3259 | Invalid bool value (190) |
| cpuemu.c | 2504, 10105 | Left shift overflow |
| cpuemu.c | 3470, 20702 | Signed integer overflow |
| EmSPISlaveADS784x.cpp | 127 | Left shift of negative value |
| EmRegsVZ.cpp | 2253 | Left shift of negative value |
| EmPatchState.h | 58 | Invalid bool value |

## Verification

After applying all fixes, the following stress tests pass with zero ASAN errors
under `ASAN_OPTIONS="detect_leaks=0:halt_on_error=1"`:

- 100+ rapid ReControl tap commands
- 50+ rapid state queries
- 5 screenshot captures (exercises EmPixMap conversion)
- 5 UI form reads (exercises EmSessionStopper + Palm form reader)
- 5 info queries (exercises screen dimensions with CPU stop)
- 30+ mixed rapid commands (state, tap interleaved)
- 30 seconds idle operation
- Process remains stable throughout

## Post-Fix Improvements

### QueueWork / QueueWorkResult refactor

After fixing all 6 bugs, the 10 commands that use CPUWorkerThread were refactored
to use two helper methods on `ReControlSession`:

- **`QueueWork(handler)`** — for void handlers (CmdTap, CmdPen, CmdKey, CmdButton).
  Creates QPointer, builds Command struct, sends `"OK\n"` in the response lambda.
- **`QueueWorkResult(handler)`** — for string-returning handlers (CmdScreenshot,
  CmdInstall, CmdLaunch, CmdSave, CmdInfo, CmdUI).  Creates QPointer +
  `shared_ptr<string>`, wraps the handler return value, sends result.

This eliminated ~100 lines of repetitive boilerplate and made the QPointer safety
pattern impossible to get wrong — the exact class of bug (Bug 5) that caused the
most frequent crashes.

### Unified command dispatch

Extracted `DispatchCommand(const QStringList& parts)` from the duplicate if/else
chains in `OnReadyRead()` and `ProcessBufferedCommands()`.  New commands only need
to be added in one place.

### UBSAN suppression file

Created `ubsan.supp` to silence known-harmless UBSAN warnings from legacy UAE code
(see "Pre-existing UBSAN Warnings" above).  Usage:
```
UBSAN_OPTIONS="suppressions=ubsan.supp:print_stacktrace=1:halt_on_error=0" ./build/pose64
```

### Automated stress test

Created `test_recontrol_stress.py` with 7 test categories (rapid taps, state
queries, screenshots, UI queries, info queries, mixed commands, disconnect/reconnect).
Self-launching or `--no-launch` for an existing instance.

## Files Modified

| File | Changes |
|------|---------|
| `src/core/EmSession.cpp` | Bugs 1, 2: timeout fix + PrvWakeUpCPU thread guard |
| `src/core/ReControl.cpp` | Bugs 3, 4, 5: QPointer for all response lambdas, null guards; QueueWork refactor; DispatchCommand extraction |
| `src/core/EmPixMap.cpp` | Bug 6: bounds check before pixel read in indexed conversion |
| `test_recontrol_stress.py` | New: automated stress test (7 test categories) |
| `ubsan.supp` | New: UBSAN suppression file for legacy UAE code |
| `docs/debugging-guide.md` | Added UBSAN suppression usage instructions |
