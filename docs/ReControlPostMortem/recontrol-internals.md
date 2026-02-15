# ReControl Internals: A Guide for the Next Expedition

This document describes the internal architecture of POSE 3.5's session
management system as we found it, what we changed to support the ReControl
socket API, what broke, what we patched, and what remains broken. It is
written for the next developer who inherits this codebase and wonders why
`install` sometimes triggers a bus error, or why `resume` has to zero every
counter in the union.

Read this before changing anything in EmSession, ReControl, or the exception
handling paths. The terrain is more interconnected than it looks.

---

## 1. Architecture Overview: Three Threads, One State Machine

POSE 3.5 runs three threads. Everything that goes wrong traces back to how
they share state.

### The CPU Thread

Created by `EmSession::CreateThread` (`EmSession.cpp:673`). Runs in
`EmSession::Run` (`EmSession.cpp:2306`). This is the m68k emulation loop:
it calls `EmSession::CallCPU`, which calls `EmCPU::Execute`, which fetches
and emulates m68k instructions one at a time.

The CPU thread holds `fSharedLock` when checking whether to suspend or
continue. It releases the lock before entering `CallCPU` and reacquires it
after `CallCPU` returns. This lock/unlock dance is the primary
synchronization mechanism:

```
Run() {
    omni_mutex_lock lock(fSharedLock);
    while (!fStop) {
        // check fSuspendState, wait if non-zero
        fSharedLock.unlock();
        CallCPU();          // runs m68k instructions
        fSharedLock.lock();
    }
}
```

The CPU thread is the only thread that may execute m68k instructions.
Everything else must stop it first.

### The FLTK Thread (UI Thread)

This is the main thread. It runs the FLTK event loop (`Fl::wait` + idle
callbacks). The FLTK idle handler is `EmApplicationFltk::HandleIdle`, which
calls `EmSession::SuspendThread(kStopNow)` to briefly pause the CPU for
screen redraws and event processing, then calls `ResumeThread` to let it
continue.

The UI thread also handles POSE's modal error dialogs (the "Bus Error" /
"Reset" / "Debug" / "Continue" dialogs). When the CPU thread needs to
display such a dialog, it calls `BlockOnDialog`, which:

1. Sets `fState = kBlockedOnUI`
2. Broadcasts `fSharedCondition`
3. Waits for the UI thread to dismiss the dialog

This is the mechanism behind the `blocked_on_ui` state visible from the
`state` command.

### The ReControl Thread (Socket Thread)

Created by `ReControl_Startup` in `ReControl.cpp:1617`. A plain pthread
that listens on a Unix domain socket, accepts connections, reads lines, and
dispatches commands.

This thread uses `EmSessionStopper` (an RAII object) to pause the CPU
before accessing emulator state. Different commands use different stop
methods:

| Stop method       | Used by                    | What it means                     |
|--------------------|----------------------------|-----------------------------------|
| `kStopNow`         | screenshot, peek during BP | Stop immediately, any state       |
| `kStopOnCycle`     | peek, poke, regs           | Stop at instruction boundary      |
| `kStopOnSysCall`   | install, launch, ui        | Stop at PalmOS trap call          |

The socket thread also uses `ResumeFromDebugger` and `ForceResume` to
resume the CPU after breakpoint stops and stuck states.

### The Mutex: `fSharedLock`

`fSharedLock` (an `omni_mutex`) protects:

- `fState` (the `EmSessionState` enum)
- `fSuspendState` (the `EmSuspendState` union)
- `fStop` (the thread-exit flag)
- `fBreakOnSysCall` (whether to break at next trap call)
- `fNestLevel` (subroutine call depth)

`fSharedCondition` (an `omni_condition` on `fSharedLock`) is used for
thread rendezvous: the CPU thread waits on it when suspended, and external
threads broadcast it to wake the CPU or signal state changes.

The event queues (`fButtonQueue`, `fKeyQueue`, `fPenQueue`) have their own
internal locks via `EmThreadSafeQueue` and can be accessed from any thread
without acquiring `fSharedLock`.

---

## 2. The Session State Machine

### EmSessionState

Four states (`EmSession.h:124`):

```
kRunning      — CPU thread is executing m68k instructions
kStopped      — CPU thread has exited (session ended or not yet started)
kSuspended    — CPU thread is paused, waiting on fSharedCondition
kBlockedOnUI  — CPU thread is waiting for a modal dialog to be dismissed
```

State transitions are guarded by `fSharedLock`. The CPU thread sets its
own state; external threads trigger state changes by modifying
`fSuspendState` and broadcasting `fSharedCondition`.

### EmSuspendState: The Counter Union

This is the heart of the suspension system and the source of most bugs.

`EmSuspendState` (`EmSession.h:108`) is a union of `EmSuspendCounters`
(a struct of bitfields) and a `uint32` (`fAllCounters`). The CPU thread
checks `fAllCounters != 0` to decide whether to suspend. If any counter
is non-zero, the CPU stops.

The counters:

| Counter                       | Width | Who increments                     | Who decrements              |
|-------------------------------|-------|-------------------------------------|-----------------------------|
| `fSuspendByUIThread`          | 4 bits | `SuspendThread` (UI/socket thread) | `ResumeThread`              |
| `fSuspendByDebugger`          | 4 bits | `ScheduleSuspendException/Error`   | `ResumeFromDebugger`        |
| `fSuspendByExternal`          | 4 bits | `ScheduleSuspendExternal`          | `ScheduleResumeExternal`    |
| `fSuspendByTimeout`           | 1 bit  | `ScheduleSuspendTimeout`           | Cleared by `ExecuteIncremental` |
| `fSuspendBySysCall`           | 1 bit  | `ScheduleSuspendSysCall`           | Cleared by `ResumeThread`   |
| `fSuspendBySubroutineReturn`  | 1 bit  | `ScheduleSuspendSubroutineReturn`  | Consumed by `ExecuteSubroutine` |

**Critical invariant:** Every increment must have a matching decrement.
If a counter is incremented but never decremented, the CPU stays
permanently suspended. This is how the "irrecoverable suspension" bug
manifests.

**The bitfield sizes matter.** `fSuspendByUIThread` and
`fSuspendByDebugger` are 4-bit signed integers. They can hold values from
-8 to 7. If incremented past 7, they overflow silently. In practice this
doesn't happen because nesting depths stay small, but it's worth knowing
the ceiling exists.

### SuspendThread / ResumeThread

`SuspendThread` (`EmSession.cpp:736`) is the mechanism external threads
use to pause the CPU. It:

1. Acquires `fSharedLock`
2. Increments a counter (which one depends on `EmStopMethod`)
3. Waits in a loop for `fState` to leave `kRunning`
4. Returns `true` if the CPU stopped in the expected way

`ResumeThread` (`EmSession.cpp:943`) decrements `fSuspendByUIThread`. If
all counters reach zero and `fState == kSuspended`, it sets
`fState = kRunning` and broadcasts.

**The kStopOnSysCall path:** This is special. `SuspendThread` doesn't
increment `fSuspendByUIThread` for `kStopOnSysCall`. Instead it sets
`fBreakOnSysCall = true` and waits for the CPU to reach a system call,
which sets `fSuspendBySysCall`. Only after confirming the CPU stopped at
a syscall does `SuspendThread` increment `fSuspendByUIThread` (line 919).
This is important because `ResumeThread` expects to find
`fSuspendByUIThread > 0`.

### The Three Stop Methods

| Method         | Mechanism                                                |
|----------------|----------------------------------------------------------|
| `kStopNow`     | Increments `fSuspendByUIThread`, CPU notices on next cycle check |
| `kStopOnCycle` | Same as `kStopNow` but `SuspendThread` validates `fState == kSuspended` (not `kBlockedOnUI`) |
| `kStopOnSysCall` | Sets `fBreakOnSysCall`, CPU enters the system-call patch and sets `fSuspendBySysCall` |

`kStopOnSysCall` can hang forever if the CPU never reaches a system call
(e.g., infinite loop in user code, or the CPU is already suspended by
another reason). This is the root cause of the "could not stop session at
syscall boundary" errors: `SuspendThread` returns `false` if the CPU
stopped for the wrong reason (e.g., `kBlockedOnUI` instead of
`kSuspended`, or `fSuspendBySysCall` not set).

---

## 3. ExecuteSubroutine: Nested Execution

`ExecuteSubroutine` (`EmSession.cpp:1153`) is the mechanism for calling
PalmOS ROM functions from POSE code. ROMStubs (like `DmFindDatabase`,
`SysUIAppSwitch`, `FrmGetActiveForm`) use `EmSubroutine` which ultimately
calls `ExecuteSubroutine` to run the m68k code that implements the trap.

### How it works

1. Save current `fSuspendState.fCounters` into `oldState`
2. Zero `fSuspendState.fAllCounters`
3. Increment `fNestLevel`
4. Release `fSharedLock` and call `CallCPU`
5. When `CallCPU` returns (because some counter became non-zero):
   - If `fSuspendBySubroutineReturn` is set: the subroutine finished, exit loop
   - If `fSuspendByDebugger` is set: something went wrong, exit loop
   - Otherwise: clear the spurious counter and re-enter `CallCPU`
6. Restore `oldState` into `fSuspendState.fCounters`

### Why fNestLevel matters

`IsNested()` returns `fNestLevel > 0`. It is checked:

- **In `EmCPU68K::Execute`** (every opcode): when nested, instruction
  breaks are skipped. This prevents ReControl breakpoints from firing
  during a ROMStub call.
- **In `EmSession::Run`** (the suspend loop): the CPU thread won't resume
  from suspension while `IsNested()` is true, even if `fAllCounters` is
  zero. This prevents the CPU thread from running ahead while the external
  thread is still inside `ExecuteSubroutine`.
- **In `CheckForBreak`**: when nested, `fSuspendByExternal` is ignored
  to let the subroutine call finish even if something tried to suspend
  externally.

### The race condition we partially fixed

**Original code** (line 1197-1209 area): When `CallCPU` returned during
a nested call with `fSuspendByUIThread` set (because the FLTK idle handler
tried to pause the CPU for a screen repaint), the original code
accumulated this counter into `oldState`:

```cpp
// ORIGINAL (buggy):
oldState.fSuspendByUIThread += fSuspendState.fCounters.fSuspendByUIThread;
fSuspendState.fCounters.fSuspendByUIThread = 0;
```

This was wrong. The FLTK idle handler pairs each `SuspendThread` with a
`ResumeThread`. If we move the count into `oldState`, the subsequent
`ResumeThread` finds `fSuspendByUIThread == 0` (because we zeroed it) and
does nothing. When `ExecuteSubroutine` finishes and restores `oldState`,
the accumulated count stays permanently, and the CPU never resumes.

**Our fix** (current code at line 1211):

```cpp
// FIXED: don't accumulate, just clear it
fSuspendState.fCounters.fSuspendByUIThread = 0;
```

We clear the counter without saving it. The paired `ResumeThread` from the
FLTK idle handler becomes a no-op (it sees `fSuspendByUIThread == 0` and
skips the decrement), which is harmless. The counter doesn't leak.

This fix prevents `fSuspendByUIThread` from growing without bound during
install/launch operations. Before this fix, rapid install/launch sequences
would accumulate `ui=3`, `ui=4`, etc. counters that could never be
decremented.

---

## 4. The Dark Corners

### BlockOnDialog

`BlockOnDialog` (`EmSession.cpp:1489`) is called from the CPU thread when
POSE detects an error (bus error, illegal instruction, memory access
violation). It:

1. Acquires `fSharedLock`
2. Calls `gDocument->ScheduleDialog` to tell the UI thread to show a dialog
3. Sets `fState = kBlockedOnUI`
4. Broadcasts `fSharedCondition`
5. Waits for the dialog to be dismissed

While `fState == kBlockedOnUI`, the CPU thread is blocked in
`fSharedCondition.wait()`. It will not respond to `fSuspendState` changes.

**Impact on ReControl:** If the CPU is blocked on a dialog,
`EmSessionStopper(kStopOnCycle)` and `EmSessionStopper(kStopOnSysCall)`
fail. `SuspendThread` sees `fState == kBlockedOnUI` and returns `false`
(for `kStopOnCycle`, line 911) or fails the
`fSuspendBySysCall` check (for `kStopOnSysCall`, line 916).

`kStopNow` succeeds because its result check (line 906) accepts both
`kSuspended` and `kBlockedOnUI`. But the CPU isn't actually stopped in
a useful way -- it's blocked in a dialog wait, and any state you read
is the state at the moment the error occurred.

**The dialog is modal in the FLTK thread.** Until the user clicks "Reset",
"Debug", or "Continue" in the FLTK window, the CPU thread stays blocked.
There is no way to dismiss this dialog from the socket API. The `reset`
command calls `ScheduleReset`, but that just sets a flag that won't be
processed until `ExecuteSpecial` runs -- which can't happen until the
dialog is dismissed and `CallCPU` is re-entered.

This is why the letter reports that "even the POSE GUI's Reset menu
doesn't work" -- the reset is scheduled but can't execute while the CPU
is blocked.

### IsNested() and exception propagation

When `CallCPU` throws an exception during a nested call
(`ExecuteSubroutine`), the exception propagates up through
`ExecuteSubroutine` and into the caller. The catch clauses in
`EmSession::Run` (`EmSession.cpp:2386`) handle `EmExceptionReset` and
`EmExceptionTopLevelAction` at the top level.

But `ExecuteSubroutine` has **no catch clauses**. If `CallCPU` throws,
the exception propagates through `ExecuteSubroutine` without restoring
`oldState` or decrementing `fNestLevel`. The `EmValueChanger<int>` for
`fNestLevel` (line 1178) will restore it via RAII, but `oldState` is lost.
The counters saved at line 1170 are never restored.

This means:

- If the caller of `ExecuteSubroutine` catches the exception (as
  ReControl's `CmdInstall` and `CmdLaunch` do with `catch(...)`), the
  session continues but with `fSuspendState` in whatever state it was
  when the exception was thrown -- typically with counters that don't
  match any paired Resume.
- The `EmSessionStopper` in the calling code will call `ResumeThread` in
  its destructor, which decrements `fSuspendByUIThread`, but the other
  counters (`fSuspendByDebugger`, `fSuspendBySysCall`, etc.) may be
  non-zero with no matching decrement.

**This is the root cause of the "irrecoverable suspension" bug.** An
exception during a nested ROM call leaves orphaned suspend counters.

### EmExceptionReset propagation

`EmExceptionReset` is POSE's primary error-reporting mechanism. When the
emulator detects a memory access violation, bus error, or illegal
instruction, the error-checking code throws `EmExceptionReset`.

The exception is caught in three places:

1. **`EmSession::Run` (line 2386):** At the top of the CPU loop. Calls
   `e.Display()` (which shows the error dialog via `BlockOnDialog`) and
   `e.DoAction()` (which calls `ScheduleReset`). This is the normal
   top-level handler.

2. **`EmSession::ExecuteIncremental` (line 1098):** Same as above.
   Used on Mac (single-threaded) path.

3. **`CTCPSocket::Idle` (`SocketMessaging.cpp:573`):** Catches
   `EmExceptionReset`, stops the CPU, and calls `e.Display()`. This is
   the legacy debugger socket's handler. Note that it catches the
   exception and calls `Display()` (showing a dialog) but does **not**
   call `e.DoAction()` -- the reset is displayed but never executed.

4. **ReControl's `CmdInstall`/`CmdLaunch` (`ReControl.cpp:408,936`):**
   Catches `...` (everything). Sends an error response to the socket
   client. Does not call `Display()` or `DoAction()`. The exception is
   swallowed.

The fourth case is the one that causes problems. When an exception occurs
during `install` or `launch`, ReControl catches it, reports an error, and
continues. But the session's internal state has been damaged by the
exception propagation through `ExecuteSubroutine` (as described above).
The emulator appears to continue running but is now in an inconsistent
state.

### Why install is unreliable

`CmdInstall` calls `EmFileImport::LoadPalmFile`, which calls numerous
PalmOS ROM functions via ROMStubs (`DmCreateDatabase`,
`DmCreateDatabaseFromImage`, `DmNewResource`, etc.). Each of these calls
goes through `ExecuteSubroutine` to execute the m68k trap implementation
in the ROM.

During these ROM calls, the emulator's memory access checking is active.
The ROM code accesses emulated RAM, and POSE checks every access against
its memory map. If the ROM code reads from an address that POSE considers
invalid (outside RAM/ROM, or in a protected region), POSE throws
`EmExceptionReset` with a bus error.

The address `0x11100004` reported in the letter is outside all mapped
regions. The most likely explanation is **heap corruption in emulated
RAM**: a previous operation corrupted a pointer in the PalmOS heap
metadata, and when `Applications.prc` (the Launcher) follows that pointer
during database enumeration after install, it reads from unmapped memory.

The intermittent nature supports this theory. The corruption depends on
the exact sequence and timing of previous operations. The `install`
command itself may not be the cause -- it may just be the operation that
triggers the already-corrupted state to be read.

We have not identified the specific corruption source. Candidates include:

- **Exception during a previous install/launch** that left the PalmOS
  heap in a partially-modified state (a new database record was allocated
  but not fully initialized before the exception interrupted the ROM code)
- **Race condition in `ExecuteSubroutine`** where the FLTK thread
  modifies emulated memory (via screen DMA or event injection) during a
  ROM call
- **A bug in the original POSE 3.5 emulation** of the m500's memory
  controller that allows certain memory-mapped register accesses to
  return garbage under specific conditions

---

## 5. What We Patched and Why

### ResumeFromDebugger (`EmSession.cpp:1972`)

**Purpose:** Allow the ReControl socket thread to resume the CPU after a
breakpoint or `ScheduleSuspendException`.

**Mechanism:** Acquires `fSharedLock`, decrements `fSuspendByDebugger` if
positive, sets `fState = kRunning` if all counters are zero, broadcasts
`fSharedCondition`.

**Why it was needed:** The original POSE had no public method for an
external thread to decrement `fSuspendByDebugger`. The legacy debugger
(DebugMgr) decremented it internally via the RPC protocol, but that
mechanism isn't available to the socket API.

### ForceResume (`EmSession.cpp:2002`)

**Purpose:** Nuclear option to recover from any stuck-suspended state.

**Mechanism:** Acquires `fSharedLock`, zeros `fSuspendState.fAllCounters`,
clears `fBreakOnSysCall`, sets `fState = kRunning` if suspended,
broadcasts `fSharedCondition`.

**Why it was needed:** When an exception during a nested ROM call leaves
orphaned suspend counters (see section 4), `ResumeFromDebugger` only
decrements `fSuspendByDebugger`. If the stuck counter is
`fSuspendByUIThread` or `fSuspendByExternal`, `ResumeFromDebugger` has
no effect. `ForceResume` zeros everything unconditionally.

**Danger:** `ForceResume` bypasses all the careful pairing of
increment/decrement that the suspend system relies on. If the FLTK idle
handler has an outstanding `SuspendThread`/`ResumeThread` pair in flight,
`ForceResume` will zero the counter out from under it, and the subsequent
`ResumeThread` will try to decrement a zero counter (which is harmless
due to the `> 0` check). But if the FLTK handler then calls
`SuspendThread` again, the count will be correct. In practice,
`ForceResume` is safe as long as it's not called during a ROMStub call.

### try/catch in CmdInstall and CmdLaunch (`ReControl.cpp:403,918`)

**Purpose:** Prevent exceptions during ROM calls from killing the socket
thread.

**Mechanism:** Wraps the `EmFileImport::LoadPalmFile` and
`DmFindDatabase`/`SysUIAppSwitch` calls in `catch(...)`.

**Why it was needed:** Without the catch, an `EmExceptionReset` thrown
during a ROM call would propagate up through the socket command handler,
through `HandleCommand`, through `HandleConnection`, and terminate the
socket thread. The emulator would continue running but the socket API
would be dead.

**What it doesn't fix:** The catch prevents the thread from dying but
doesn't restore the session to a consistent state. The orphaned suspend
counters remain. The emulated PalmOS heap may be in a half-modified
state. The next command may work, or may fail in a new way.

### ExecuteSubroutine fSuspendByUIThread fix (`EmSession.cpp:1211`)

**Purpose:** Prevent `fSuspendByUIThread` from accumulating during nested
calls.

**The fix is documented in section 3** above. The key insight: don't save
`fSuspendByUIThread` into `oldState` because the paired `ResumeThread`
expects to find the counter in the live state, not in a saved copy.

### Enhanced state command (`ReControl.cpp:780`)

**Purpose:** Give socket clients diagnostic visibility into the suspend
system.

**What it adds:** When any suspend counter is non-zero, the `state`
response includes a colon-suffixed reason label and all raw counters:

```
OK suspended:debugger ui=0 dbg=1 ext=0 timeout=0 syscall=0 subret=0
```

This was essential for diagnosing the suspension bugs. Without it, the
only information available was "suspended" with no indication of why.

---

## 6. What Remains Broken

### Exception swallowing in CmdInstall/CmdLaunch

The `catch(...)` blocks in `CmdInstall` and `CmdLaunch` catch every
exception, including `EmExceptionReset`. They report an error to the
socket client, but the session continues with corrupted state. There is
no attempt to:

- Restore `fSuspendState` to its pre-call value
- Reset the emulated PalmOS heap
- Determine whether the session is still usable

A better approach would be to:

1. Save `fSuspendState` before the ROM call
2. In the catch block, restore the saved state
3. Schedule a soft reset (`ScheduleReset(kResetSoft)`) to let PalmOS
   recover its heap

But this is non-trivial because `ExecuteSubroutine` modifies
`fSuspendState` internally, and the exception may leave `fNestLevel`,
`fBreakOnSysCall`, and other session variables in inconsistent states.

### The bus error mystery

The intermittent bus error at `0x11100004` during install remains
undiagnosed. We know:

- It occurs inside `Applications.prc` (the Launcher), not inside the
  install code itself
- The address is outside all mapped memory regions
- It's intermittent and not correlated with the PRC file content
- It may be caused by heap corruption from a previous operation

To diagnose this properly, someone would need to:

1. Enable POSE's memory access logging (`LogAppendMsg` calls in
   EmBankMapped, EmBankSRAM, etc.)
2. Trace the exact ROM function call chain that leads to the bad access
3. Examine the PalmOS heap metadata at the time of the error
4. Determine whether a specific previous operation corrupts the heap

### SocketMessaging's silent catch

`CTCPSocket::Idle` (`SocketMessaging.cpp:563`) catches `EmExceptionReset`
from the legacy debugger protocol and calls `e.Display()` but not
`e.DoAction()`. This means the error dialog is shown (blocking the CPU)
but the reset is never executed.

This is not a ReControl bug -- it's in the original POSE code -- but it
means that if the legacy debugger socket is active alongside ReControl,
exceptions may be caught and half-handled by the legacy code before
ReControl sees them.

In practice this doesn't matter because we don't use the legacy debugger
socket. But if someone enables it alongside ReControl, they will see
confusing behavior.

### BlockOnDialog is unreachable from the socket

When the emulator shows a modal error dialog, the CPU is blocked in
`BlockOnDialog` waiting for the UI thread to dismiss it. There is no
socket command to dismiss this dialog. The `reset` command schedules a
reset but can't force the dialog to close.

This means that if a bus error or other emulator-detected error occurs
during normal CPU execution (not during a ReControl-initiated ROM call),
the emulator wedges on the dialog and the socket API becomes unresponsive
for any command that needs to stop the CPU.

The only recovery is to interact with the FLTK GUI to dismiss the dialog,
or kill the process.

### ForceResume can't fix kBlockedOnUI

`ForceResume` zeros all suspend counters and sets `fState = kRunning` if
`fState == kSuspended`. But if `fState == kBlockedOnUI`, it does nothing
to the state. The CPU thread is blocked in `fSharedCondition.wait()`
inside `BlockOnDialog`, and changing the counters doesn't wake it up
from that particular wait -- it's waiting for the dialog result, not for
the counters to change.

---

## 7. Warnings for Future Work

### Don't add more catch(...) blocks without state cleanup

Catching exceptions from `ExecuteSubroutine` without restoring
`fSuspendState` creates orphaned counters. If you must catch, save the
full `EmSuspendState` before the ROM call and restore it in the catch
block. But be aware that `fNestLevel` is managed by `EmValueChanger`
(RAII) inside `ExecuteSubroutine`, so it should be restored by stack
unwinding -- verify this is actually happening.

### Don't assume the CPU is in a known state after an exception

After catching an exception from a ROM call, the emulated CPU registers,
stack pointer, and PalmOS globals may be in an inconsistent state. The
ROM function may have been halfway through modifying the database
manager's internal data structures. The safest action is to schedule a
reset.

### Don't call ROMStubs from the socket thread without kStopOnSysCall

ROMStubs use `EmSubroutine` which calls `ExecuteSubroutine`. This
re-enters the CPU loop on the calling thread. If you call a ROMStub
from the socket thread without first stopping the CPU at a syscall
boundary, you'll have two threads executing m68k instructions
simultaneously. Nothing protects against this, and the results are
undefined (typically heap corruption followed by a crash).

The pattern is always:

```cpp
EmSessionStopper stopper(gSession, kStopOnSysCall);
if (!stopper.CanCall()) {
    // Cannot proceed — CPU is not at a safe point
    return;
}
// Now safe to call ROMStubs
LocalID id = DmFindDatabase(0, "MyApp");
```

### fSuspendByUIThread is 4 bits signed

It holds values from -8 to 7. In the original POSE, it never exceeded
1 or 2. With ReControl adding another source of `SuspendThread` calls,
the maximum value is higher. If you add more sources of `SuspendThread`
calls (e.g., a second socket, a scripting engine), be aware of the
overflow risk.

### SuspendThread(kStopOnSysCall) can hang

If the CPU is in a state where it never reaches a system call (infinite
loop in user code, halted by a breakpoint, or already suspended by
another counter), `SuspendThread(kStopOnSysCall)` will wait forever.
There is no timeout.

ReControl doesn't implement a timeout either. If `CmdInstall` or
`CmdLaunch` hangs because the CPU never reaches a syscall, the socket
connection hangs and the only recovery is to disconnect the client.

A timeout in `SuspendThread` or in the ReControl command handlers would
be a significant improvement.

### The FLTK idle handler and ReControl compete for SuspendThread

Both the FLTK idle handler and the ReControl socket thread call
`SuspendThread` to pause the CPU. They share the same counter
(`fSuspendByUIThread`) and the same lock. There is no priority mechanism.

In practice, the FLTK handler pauses the CPU for ~1ms at a time for
screen updates. ReControl pauses it for the duration of a command
(typically a few ms for peek/regs, potentially 100ms+ for install).
These interleave without problems most of the time.

The issue arises during `ExecuteSubroutine`. The FLTK handler's
`SuspendThread` can fire while the ReControl thread is inside a nested
ROM call. This triggers the `fSuspendByUIThread` handling inside
`ExecuteSubroutine` (which we fixed -- see section 3). Before our fix,
this caused counter accumulation. After our fix, the counter is cleared
and the FLTK handler's `ResumeThread` is a no-op.

### The omni_thread library is old and fragile

POSE uses the omni_thread library from omniORB for threading. This
library predates C++11 `std::thread` and `std::mutex`. It works, but its
error handling is minimal -- most errors are silent. If a mutex operation
fails, you won't know.

The `omni_mutex_lock` / `omni_mutex_unlock` RAII objects are exception-
safe (they release in their destructors), which is important given how
many exceptions fly through this code.

### Don't modify fState without holding fSharedLock

Several code paths read `fState` without the lock (it's a simple enum
and reads are atomic on all modern architectures). But **writing** `fState`
must be done under `fSharedLock` with a subsequent broadcast of
`fSharedCondition`, or the CPU thread will never notice the change.

---

## Appendix: File Reference

| File | What it contains |
|------|-----------------|
| `EmSession.h` | `EmSuspendCounters`, `EmSuspendState`, `EmSessionState`, `EmStopMethod`, `EmSession`, `EmSessionStopper` |
| `EmSession.cpp` | Session lifecycle, CPU thread (`Run`), `SuspendThread`/`ResumeThread`, `ExecuteSubroutine`, `BlockOnDialog`, `ForceResume`, `ResumeFromDebugger` |
| `ReControl.cpp` | Socket listener, command handlers (`CmdInstall`, `CmdLaunch`, `CmdState`, etc.), PNG encoder |
| `ReControlTrace.h/cpp` | Trap tracing, breakpoint management, `continue`/`step` implementation |
| `EmException.h` | `EmExceptionReset`, `EmExceptionTopLevelAction`, `EmExceptionEnterDebugger` |
| `SocketMessaging.cpp` | Legacy TCP debugger socket, `CTCPSocket::Idle` catch clause |
| `ROMStubs.h/cpp` | C++ wrappers for PalmOS trap calls (use `EmSubroutine`/`ExecuteSubroutine`) |
| `EmCPU68K.cpp` | m68k CPU loop, opcode execution, `CheckForBreak` integration |
| `EmPalmOS.cpp` | System call handler, ReControlTrace hook point |
