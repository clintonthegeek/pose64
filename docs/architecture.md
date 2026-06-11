# POSE64 Architecture Guide

**Purpose:** Prevent wasted time. This document explains how the emulator
actually works, what's safe, what's dangerous, and the hard-won lessons from
bugs we've already fixed. Read this before touching threading, event delivery,
ROM interaction, or the view layer.

---

## Table of Contents

1. [Threading Model](#threading-model)
2. [The CPU Loop](#the-cpu-loop)
3. [Event Delivery (Pen, Key, Button)](#event-delivery)
4. [PuppetString and ROM Patching](#puppetstring-and-rom-patching)
5. [ExecuteSubroutine (Calling ROM Functions)](#executesubroutine)
6. [The STOP Instruction](#the-stop-instruction)
7. [CPU Suspension (EmSessionStopper)](#cpu-suspension)
8. [ReControl and CPUWorkerThread](#recontrol-and-cpuworkerthread)
9. [Screen and Painting](#screen-and-painting)
10. [Qt Integration](#qt-integration)
11. [Hardware Emulation (EmRegs)](#hardware-emulation)
12. [Do-Not-Do List](#do-not-do-list)
13. [How To Do Things Safely](#how-to-do-things-safely)
14. [Key File Map](#key-file-map)

---

## Threading Model

Three threads. This is the single most important thing to understand.

```
Main Qt Thread (T0)           CPU omni_thread (T1)          CPUWorkerThread (T9)
  - Qt event loop               - m68k instruction loop       - ReControl command handlers
  - HandleIdle() at ~10Hz       - EmSession::Run()            - EmSessionStopper calls
  - Mouse/key input              → CallCPU()                  - Blocking operations
  - Window painting               → EmCPU68K::Execute()       - Does NOT run m68k code
  - ReControl TCP accept         - CycleSlowly() for HW
  - Dialog display               - PuppetString headpatch
```

**Synchronization primitives:**
- `fSharedLock` / `fSharedCondition` (omni_mutex/condition) — protects
  `fSuspendState`, `fState`, `fStop`. The CPU thread holds this lock while
  running; it releases it only when suspended or checking state.
- `std::atomic<uint32>` — used for `fButtonState`, `fButtonTaps`,
  `fEmulationSpeed`, `fEffectiveClockFreq`. Lock-free.
- `EmThreadSafeQueue` — mutex-protected FIFO for `fKeyQueue`, `fPenQueue`.

**The cardinal rule:** The CPU thread is the only thread that executes m68k
instructions. The only thread that touches CPU registers (`regs.*`). The only
thread that calls ROM stubs via ExecuteSubroutine. Never violate this.

---

## The CPU Loop

`EmCPU68K::Execute()` is a tight `while(1)` loop that fetches and executes
one 68K instruction per iteration.

```
Execute() {
    while (1) {
        opcode = fetch(PC)
        functable[opcode]()          // execute one instruction

        CYCLE(sleeping, cycles)      // advance timers, maybe CycleSlowly

        if (spcflags) {
            if (SPCFLAG_STOP)    → ExecuteStoppedLoop()
            if (SPCFLAG_INT)     → ProcessInterrupt()
            if (SPCFLAG_SUSPEND) → return (back to Run() loop)
        }
    }
}
```

**The CYCLE macro** (`EmCPU68K.cpp:126`):
- Always calls `EmHAL::Cycle()` to advance hardware timers — even during
  nested ExecuteSubroutine calls. Without this, STOP during nested calls
  would hang because no timer interrupt would ever fire.
- Calls `CycleSlowly()` every 32,768 instructions (or every iteration when
  sleeping). CycleSlowly is NOT called during nested execution.
- CycleSlowly handles: speed throttling, button polling, UART updates, RTC
  alarms, and screen updates.

---

## Event Delivery

### Pen Events

```
Qt mousePressEvent (EmWindowQt, main thread)
  → translate Qt coords to Palm coords (0-159)
  → EmSession::PostPenEvent() — adds to fPenQueue (thread-safe)

Later, on CPU thread:
  → App calls EvtGetEvent → SysEvGroupWait trap
  → SysEvGroupWait headpatch fires → PuppetString()
  → PuppetString checks fPenQueue, dequeues event
  → Calls StubAppEnqueuePt() (ROM stub via ExecuteSubroutine)
  → Sets callROM = kSkipROM + PrvForceNilEvent()
  → SysEvGroupWait returns immediately
  → EvtGetEvent finds the pen event in ROM's queue
```

**Critical:** Pen events are delivered through PuppetString's software path,
NOT through the hardware digitizer. The ADS784x SPI slave returns 0 for pen
X/Y channels. The ROM's pen interrupt handler is never involved.

**Thread safety of PostPenEvent (task 1.7):** `PostPenEvent` is called from
both the Qt main thread (mouse handler) and the CPUWorkerThread (`tap`/`pen`
commands). `fLastPenEvent` (the dedup guard) is guarded by `fPenEventLock`
(an `omni_mutex`) to eliminate the two-writer data race. `fPenQueue` is
already `EmThreadSafeQueue` (lock-free FIFO) — no change needed there.

### Key Events

Same flow as pen events but through `fKeyQueue` and `StubAppEnqueueKey()`.

### Hardware Button Events

Different from pen/key — these go through actual hardware emulation:

```
Qt mousePressEvent on skin button region (main thread)
  → gSession->SetButtonDown(kElement_PowerButton) — atomic write

CycleSlowly (CPU thread, every 32K instructions):
  → PollButtonChanges() detects 0→1 transition
  → EmRegsXX::ButtonEvent() sets PORT register bits
  → Hardware interrupt fires (keyboard interrupt, level 4)
  → ROM ISR reads port register, generates key event
```

Buttons use state-based atomic writes, not event queues. The CPU polls for
edge transitions. There's a 5-cycle cooldown between changes to give the ROM
time to acknowledge each one.

---

## PuppetString and ROM Patching

PuppetString is how POSE injects events into Palm OS. It's a **headpatch**
on `SysEvGroupWait` — the function Palm OS calls when it's idle and waiting
for events.

**File:** `src/core/Patches/EmPatchMgr.cpp:921`

**How headpatches work:**
- When the 68K CPU executes a system trap (TRAP #F), the patch system
  intercepts it before the ROM function runs.
- The headpatch returns `kExecuteROM` (let the ROM function run normally)
  or `kSkipROM` (skip it, the headpatch handled everything).
- `EmPatchModuleSys.cpp` contains headpatches for ~30 system functions.

**PuppetString's logic:**
1. Check `EmLowMem::GetEvtMgrIdle()` — if the event manager isn't idle,
   don't inject events.
2. If nested (inside ExecuteSubroutine), skip ROM and force nil event —
   never let a nested call sleep.
3. If Gremlins or event playback is active, handle those (always set
   `clearTimeout = true`).
4. Otherwise (interactive mode) — **as actually implemented at HEAD `460449e`**:
   - If key events queued: dequeue, call `StubAppEnqueueKey()`, then **fall
     through** (does NOT `kSkipROM`, does NOT set `clearTimeout`).
   - If pen events queued: dequeue, call `StubAppEnqueuePt()`, then **fall
     through** (does NOT `kSkipROM`, does NOT set `clearTimeout`).
   - If app switch pending: do it, set `clearTimeout = true`.
   - `clearTimeout = true` is set **only** for Replay / Hordes / app-switch —
     **NOT** for plain interactive pen/key delivery, so the real ROM
     `SysEvGroupWait` runs and may sleep with an infinite timeout.
   > **Correction (2026-06-10):** earlier text claimed interactive mode always
   > set `clearTimeout`/`kSkipROM`. That "poll-always" design is the deferred
   > 2026-03-13 patch (Phase 2 approach **A**), NOT current behavior. See
   > `docs/superpowers/plans/2026-06-10-phase2-planning-handoff.md`.

   > **Phase 2 decisions (2026-06-10, handoff §10):** the input-response
   > contract is decided — `tap`/`pen`/`key`/`type` will block ≤2 s on a
   > delivery counter and return `OK delivered` / `ERR pending` / `ERR busy`
   > truthfully (Q-ACK option a; `button` keeps its hardware-ISR contract).
   > **Mechanism DECIDED 2026-06-10 (2.2 checkpoint): B + C.** The wake
   > mechanism is robust **B** — a STOP-exit `EvtWakeup` hook on the CPU
   > thread inside `ExecuteStoppedLoop` (handoff §10 Q-B3, experiment branch
   > `phase2-experiment-B` @ `a09598d`). Approach **A** (poll-always) is
   > **rejected/dead** (handoff §11.3): it cannot wake an already-asleep
   > guest and floods awake apps with nilEvents. The B experiment passed
   > (handoff §12.1): idle delivery 0→100/100, idle p50 220 ms vs natural
   > 297 ms, zero measured idle-CPU cost, TSAN clean of any
   > hook-implicating report. B was chosen over natural-delivery+C — which
   > also works for the *polling* built-ins — because only B guarantees a
   > ≤1-tick delivery bound for true-`evtWaitForever` apps, the AI-driving
   > target the built-ins cannot exhibit. The hook is **landed to master
   > and all losers deleted (R2) in plan Task 8**; until then it lives only
   > on the experiment branch. See
   > `docs/superpowers/plans/2026-06-10-phase2-input-delivery.md` Task 5.

**`clearTimeout`:** When true, changes SysEvGroupWait's timeout from 0
(infinite/wait forever) to -1 (no wait/return immediately). This prevents
the CPU from entering STOP and ensures PuppetString fires on every
EvtGetEvent call.

**`PrvForceNilEvent()`:** Sets D0 register to 4 (CJ_WRTMOUT), making
SysEvGroupWait return a timeout error. EvtGetEvent interprets this as
"no event" and returns a nilEvent to the app.

---

## ExecuteSubroutine

Lets the emulator call ROM functions as if they were C functions.

**File:** `src/core/EmSubroutine.cpp`

**How it works:**
1. Save all 68K registers.
2. Push parameters onto the 68K stack (A7).
3. Set PC to the ROM function's address (via TRAP #F setup).
4. Run the CPU loop with `fSuspendBySubroutineReturn` — it stops when
   the function returns (RTS).
5. Read return value from D0.
6. Restore all registers.

**Used by:** ROM stubs in `ROMStubs.cpp` (EvtEnqueuePenPoint,
DmCreateDatabase, SysAppLaunch, etc.) and PuppetString.

**Constraints:**
- Must run on the CPU thread. The CPU thread is the only thread that
  can execute 68K instructions.
- Cannot be called while the CPU is in STOP state. There's an assert:
  `EmAssert(oldRegs.stopped == 0)`.
- Increments `fNestLevel`. CycleSlowly is skipped while nested.
  Timers still advance via EmHAL::Cycle.
- Can nest (ROM stub calls another ROM stub). Each level saves/restores
  registers independently.

---

## The STOP Instruction

The 68K STOP instruction puts the CPU in low-power mode until an interrupt
fires. Palm OS uses this inside SysEvGroupWait when there are no events.

**`ExecuteStoppedLoop()` (`EmCPU68K.cpp:761`):**
```
while (SPCFLAG_STOP is set) {
    CYCLE(sleeping=true, ...)     // advance timers, call CycleSlowly
    check for interrupts          // timer, keyboard, etc.
    if (interrupt level > mask)   // interrupt fires!
        ProcessInterrupt()        // clears STOP, jumps to ISR
    check for suspension          // EmSessionStopper requested?
    sleep briefly (wall clock)    // throttle to emulated speed
}
```

When an interrupt fires during STOP, the CPU wakes up, the ISR runs, and
execution continues. For SysEvGroupWait, the ISR typically returns to the
wait loop inside SysEvGroupWait, which checks if its event group was
signaled. If not, it re-enters STOP.

**Idle sleep status (corrected 2026-06-10):** Earlier text here claimed the CPU
"doesn't sleep anymore" because interactive mode always sets `clearTimeout`. That
is **NOT** current behavior (HEAD `460449e`): interactive pen/key delivery leaves
`clearTimeout = false`, so `SysEvGroupWait` can take an infinite timeout and the
CPU can enter STOP — which is precisely the "guest asleep ⇒ event undelivered"
failure mode Phase 2 must fix. How much the guest actually sleeps at idle is
unmeasured pending Phase 2's idle-CPU harness. The poll-always "never sleep"
design is Phase 2 approach **A** (deferred 2026-03-13 patch), not today's code.

---

## CPU Suspension (EmSessionStopper)

RAII object to briefly pause the CPU thread from another thread.

**File:** `src/core/EmSession.h:709`

```cpp
EmSessionStopper stopper(gSession, kStopOnCycle);
if (stopper.Stopped()) {
    // CPU is suspended — safe to read registers, memory, etc.
    // Destructor resumes the CPU.
}
```

**Stop methods:**
- `kStopNow` — suspend immediately (sets `fSuspendByUIThread`).
- `kStopOnCycle` — suspend at end of current instruction cycle. Returns
  false (no-op) when the CPU is `kBlockedOnUI` — the failure path is
  side-effect-free (task 1.1 fix; prior to that, `fSuspendByUIThread` leaked
  permanently on this path).

  **Deferral during nested subroutines (landmine #10 fix, 2026-06-10):**
  if the CPU thread is inside a nested `ExecuteSubroutine` (a host-initiated
  ROM call — tailpatches, PuppetString, ROM stubs) when a
  `kStopNow`/`kStopOnCycle` request arrives, the request is DEFERRED: the
  ROM call always runs to completion (`CheckForBreak` masks
  `fSuspendByUIThread` while `IsNested()`), and the stop takes effect at
  the outer Execute loop right after the trap dispatch finishes
  (`ExecuteSubroutine` re-arms `CheckAfterCycle` on exit). The counter
  stays live throughout, so stopper timeouts and `ResumeThread` accounting
  remain correct. The previous behavior — breaking out of the nested call —
  made ROM stubs return garbage register values to their host callers
  (landmine #10: intermittent `MemoryMgr.c` fatal alerts under app-switch
  churn with concurrent polling). The deferral wait is bounded: ROM stubs
  are short, timers advance at every nesting depth (a nested STOP always
  clears), and a nested dialog flips `fState` off `kRunning`, which
  satisfies waiting stoppers. This restores the upstream POSE 3.5
  semantics.
- `kStopOnSysCall` — suspend at the next system call boundary. **BLOCKS
  the calling thread** until the CPU reaches a syscall. Can deadlock if
  the CPU never reaches one (e.g., tight loop, sleeping in STOP with no
  events, nested ExecuteSubroutine).
- `kStopNone` — no-op.

**Timeout:** `SuspendThread(how, timeoutMs)` accepts a timeout. If the CPU
doesn't reach the requested state within the timeout, it returns false.

**Failure is side-effect-free (task 1.1):** When `SuspendThread` returns
false for `kStopNow`/`kStopOnCycle`, the `fSuspendByUIThread` counter is
guaranteed to be unchanged. `EmSessionStopper` correctly does NOT call
`ResumeThread` on failure — both sides of the contract are now consistent.
`ForceReset` also clears `fSuspendByUIThread` as a last-resort recovery
guarantee.

**Bounded timeouts (task 1.2):** `useTimeout = (timeoutMs > 0)` — the old
`&& how == kStopOnSysCall` restriction is removed. `kCmdWorkerCycle` and
`kCmdAdaptive` dispatch pass 5000ms so a nested-ROM deadlock (CPU in
`IsNested()` state, wait-loop livelock) times out cleanly instead of parking
the worker thread forever. The `kStopOnCycle` timeout path also balances
`fSuspendByUIThread` (consistent with the 1.1 fix).

---

## ReControl and CPUWorkerThread

### ReControl

TCP debug/control protocol. Text-based, `\n`-terminated, localhost only.

**Files:** `src/core/ReControl.cpp`, `ReControlCmds_*.cpp`

**Ports:** 6416 (dev), 6425 (secondary), 6427 (testing)

**Command categories (dispatch table at ReControl.cpp:101):**

| Category | What happens | Example commands |
|----------|-------------|-----------------|
| `kCmdImmediate` | Runs on main thread, no CPU sync | `state`, `log`, `check` |
| `kCmdWorkerDirect` | Runs on CPUWorkerThread, fire-and-forget | `tap`, `pen`, `key`, `button` |
| `kCmdWorkerCycle` | Stops CPU at cycle boundary, returns result | `ui`, `break`, `watch` |
| `kCmdWorkerSysCall` | Stops CPU at syscall, returns result | `launch`, `export`, `apps` |
| `kCmdWorkerRaw` | Handler manages its own CPU state | `install`, `screenshot` |
| `kCmdAdaptive` | Direct if blocked_on_ui, else stop at cycle | `peek`, `poke`, `regs`, `backtrace` |
| `kCmdCustom` | Handler manages everything | `reset`, `sleep`, `dialog` |

### CPUWorkerThread

**File:** `src/core/CPUWorkerThread.h/cpp`

Dedicated QThread for ReControl handlers that block. The problem it solves:
`EmSessionStopper(kStopOnSysCall)` blocks the calling thread until the CPU
reaches a syscall. If you call this on the main thread, the Qt event loop
freezes — no painting, no input, no dialog responses.

CPUWorkerThread runs blocking handlers off the main thread. Responses are
delivered back to the main thread via `QMetaObject::invokeMethod(...,
Qt::QueuedConnection)`.

**Bounded shutdown (task 1.4):** `CPUWorkerThread::shutdown()` now sets
`fShouldStop = true`, queues `CMD_SHUTDOWN`, and calls `wait(8000)` with a
`terminate()` + `wait(2000)` fallback. This prevents the main thread from
blocking forever when `load`, `reset`, or `quit` triggers a shutdown while a
handler is in flight. After 1.2, handlers self-release within 5000ms so the
8s limit is never hit in normal operation.

---

## Screen and Painting

```
Emulated LCD framebuffer (memory-mapped)
  → EmScreen::GetBits() reads framebuffer, tracks dirty regions
  → CycleSlowly() calls PaintScreen() when dirty
  → EmWindowQt::HostPaintLCD() converts bitmap to QImage
  → QPainter renders to window (skin + LCD composite)
```

**Pixel formats:** 1-bit, 2-bit, 4-bit indexed, 16-bit direct (RGB565).

**Skin rendering:** Device bezel/buttons rendered behind LCD. LCD composited
on top. Button press feedback via HostRectFrame overlay.

---

## Qt Integration

**Files:** `src/platform/EmApplicationQt.cpp`, `EmWindowQt.cpp`, `EmDlgQt.cpp`

- `EmApplicationQt` sets up a QTimer that fires `HandleIdle()` at ~10Hz.
- `HandleIdle()` drives screen updates and dialog processing.
- `EmWindowQt` is a QWidget subclass handling paint, mouse, and key events.
- Dialogs use `BlockOnDialog()` (CPU thread) with a state-machine lifetime
  contract: `BlockOnDialog` never returns until the scheduled `EmActionDialog`
  is either cancelled-while-queued or has finished running. UI-side entry/exit
  is `BeginDialogAction`/`EndDialogAction` (guarded by `fSharedLock`).
  `fDialogActionState` tracks the lifecycle: None → Queued → Running → Done
  (or Cancelled if reset fires while still Queued). This prevents UAF when
  `reset` is issued while a deferred-error dialog is queued or showing.

---

## Hardware Emulation

**EmRegs hierarchy:**
```
EmRegs (abstract)
  ├── EmRegs328    (DragonBall 328 — PalmPilot/III)
  ├── EmRegsEZ     (DragonBall EZ — Palm V, IIIc)
  └── EmRegsVZ     (DragonBall VZ — Palm m500/m505)
```

Each variant overrides `CycleSlowly()` to handle:
- Button polling (PollButtonChanges → ButtonEvent → port register → interrupt)
- UART state updates
- RTC alarm checks

**Timers:** Hardware timer registers are incremented by `EmHAL::Cycle()` on
every instruction. Timer interrupts fire at level 6 and are the primary
mechanism that wakes the CPU from STOP.

**ADS784x SPI slave:** Emulates the touchscreen ADC. Returns 0 for pen X/Y
channels. Battery channel returns a value derived from Palm OS battery
globals. Pen events are NOT delivered through this hardware path.

---

## Do-Not-Do List

These are hard-won lessons. Every item on this list represents a bug that
was actually shipped or a fix attempt that failed.

### Threading

1. **DO NOT call ExecuteSubroutine from the main thread.** It runs 68K
   instructions, which can only happen on the CPU thread. Calling from the
   main thread would mean two threads executing 68K simultaneously → crash.

2. **DO NOT call EmSessionStopper(kStopOnSysCall) from the main thread.**
   It blocks until the CPU reaches a syscall. If the CPU is nested in
   ExecuteSubroutine or sleeping in STOP, this blocks the main thread
   indefinitely → GUI freeze. Use CPUWorkerThread instead.

3. **DO NOT compute timeout deadlines inside while loops.** Spurious
   condition variable wakeups reset the deadline and the timeout never
   fires. Compute the absolute deadline ONCE before the loop. (See
   `docs/stability-findings.md`, Bug 1.)

4. **DO NOT capture raw pointers in lambdas that execute later.** Qt objects
   can be destroyed before the lambda fires. Use `QPointer<T>` and check
   for null. (See `docs/stability-findings.md`, Bugs 3 and 5.)

5. **DO NOT access `gSession` in deferred callbacks without null-checking.**
   Session transitions (load, reset) destroy and recreate gSession. A
   queued lambda may fire with a stale pointer.

### Event Delivery

6. **DO NOT fire the pen interrupt (hwrXXXIntHiPen) to deliver pen events.**
   The ROM's pen interrupt handler reads the ADS784x SPI slave and generates
   its own pen events from the hardware reading. Even with ADS784x returning
   0, the handler generates spurious events at wrong coordinates. The ROM's
   pen interrupt handler and PuppetString's software delivery are two
   independent event paths that conflict with each other.

7. **DO NOT try to set ADS784x pen X/Y to real coordinates.** The coordinate
   space mapping between raw ADC values and screen coordinates depends on
   the ROM's calibration data, which varies by ROM version, device, and
   user calibration. It's not a simple linear scale. The original POSE
   never used this path.

8. **DO NOT call PrvWakeUpCPU / EvtWakeup from the UI thread.** The old
   implementation used `EmSessionStopper(kStopOnSysCall)` which blocks the
   main thread. This was removed in commit 7b51f89 for exactly this reason.

9. **DO NOT let SysEvGroupWait sleep with an infinite timeout in interactive
   mode.** If the CPU is sleeping in STOP and a pen event arrives, there's
   no mechanism to wake it and cause SysEvGroupWait to return. Always set
   `clearTimeout = true` so SysEvGroupWait returns immediately and
   PuppetString fires on every EvtGetEvent call.
   **(Status note 2026-06-10:** this states the *intended* end-state; the current
   tree does NOT yet implement it for interactive pen/key delivery — see the
   Event Delivery correction above. "Always set `clearTimeout`" is Phase 2
   approach **A**, still an open decision vs. approach **B** targeted-wake.)**

10. **DO NOT inject events when `EmLowMem::GetEvtMgrIdle()` returns false.**
    The event manager is in the middle of processing. PuppetString already
    checks this. Bypassing this check causes events to be injected at wrong
    times.

### ROM Interaction

11. **DO NOT call ROM stubs while the CPU is in STOP.** There's an assert:
    `EmAssert(oldRegs.stopped == 0)`. ExecuteSubroutine saves and restores
    registers, but a stopped CPU cannot meaningfully execute subroutines.

12. **DO NOT skip CycleSlowly's nesting check.** CycleSlowly is intentionally
    not called during nested ExecuteSubroutine. Running CycleSlowly during
    nested calls would trigger button polling, screen updates, and
    throttling at the wrong time.

13. **DO NOT assume ROM functions are reentrant.** Palm OS system functions
    are generally not reentrant. Calling a ROM stub while the CPU is
    already inside the same function (or one that shares global state)
    will corrupt Palm OS internal state.

16. **DO NOT post actions that reference CPU-thread stack data without a
    lifetime handshake.** `ScheduleDialog` posts an `EmActionDialog` holding
    references into `BlockOnDialog`'s stack frame. Without the
    `BeginDialogAction`/`EndDialogAction` handshake, `reset` can unwind the
    stack while the UI thread still holds those references → UAF. Any future
    action that captures a pointer to stack-allocated data must use the same
    pattern: state machine under `fSharedLock`, CPU waits until action is
    done or cancelled.

### Hardware Emulation

14. **DO NOT try to signal the event group directly by writing to memory.**
    The memory layout of kernel data structures varies between ROM versions.
    Any direct manipulation would be fragile and ROM-version-dependent.

15. **DO NOT add interrupt sources without understanding their ISR.** Every
    interrupt has a ROM handler. Firing an interrupt means the ROM's ISR
    runs. If the ISR has side effects (reading hardware, modifying state),
    those side effects happen. The pen interrupt (level 5) ISR reads the
    ADS784x and generates pen events — which is why firing it to "wake up"
    the CPU caused spurious clicks.

---

## How To Do Things Safely

### To deliver user input to Palm OS:

**Pen/key events:** Post to `fPenQueue`/`fKeyQueue` via
`PostPenEvent()`/`PostKeyEvent()` from any thread. PuppetString will pick
them up on the next SysEvGroupWait call and inject them via ROM stubs.
PuppetString handles skipping SysEvGroupWait (`kSkipROM`) and preventing
sleep (`clearTimeout = true`).

**Hardware buttons:** Use `SetButtonDown()`/`SetButtonUp()`/`SetButtonTap()`
from any thread. These are atomic state writes. CycleSlowly polls for edge
transitions and generates hardware interrupts.

### To stop the CPU briefly:

From CPUWorkerThread (recommended):
```cpp
EmSessionStopper stopper(gSession, kStopOnSysCall, 5000);
if (stopper.Stopped()) { /* safe to read state */ }
```

From the main thread (only kStopOnCycle or kStopNow):
```cpp
EmSessionStopper stopper(gSession, kStopOnCycle);
if (stopper.Stopped()) { /* quick operation only */ }
```

**Never use kStopOnSysCall from the main thread.**

### To add a new ReControl command:

1. Add the handler function in the appropriate `ReControlCmds_*.cpp` file.
2. Add an entry to the command table in `ReControl.cpp:101`.
3. Choose the right category:
   - Read-only, no CPU state needed → `kCmdImmediate`
   - Needs CPU stopped at safe point → `kCmdWorkerCycle` or `kCmdWorkerSysCall`
   - Injects input → `kCmdWorkerDirect`
   - Complex state management → `kCmdCustom`
4. If blocking, let CPUWorkerThread handle it — don't block the main thread.

### To add a new ROM patch (headpatch/tailpatch):

1. Add the function to `EmPatchModuleSys.cpp` (or appropriate module).
2. Register it in the module's patch table.
3. Return `kExecuteROM` to let the ROM function run, or `kSkipROM` to
   handle it entirely in the headpatch.
4. If you skip the ROM, set the return value in D0 yourself.

### To read emulator state safely:

- `gSession->GetSessionState()` — thread-safe, returns current state.
- If state is `kBlockedOnUI`, CPU registers and memory are stable — you
  can read them directly without stopping the CPU.
- Otherwise, use EmSessionStopper to pause the CPU first.

---

## Key File Map

### Core Session & Threading
| File | Role |
|------|------|
| `EmSession.h/cpp` | Session lifecycle, threading, event queues, suspension |
| `CPUWorkerThread.h/cpp` | Dedicated thread for blocking ReControl handlers |

### CPU Emulation
| File | Role |
|------|------|
| `Hardware/EmCPU68K.h/cpp` | 68K execution loop, STOP handling, interrupts |
| `Hardware/EmCPU.h/cpp` | CPU abstraction layer |

### Hardware
| File | Role |
|------|------|
| `Hardware/EmRegs328.cpp` | DragonBall 328 register emulation (2876 lines) |
| `Hardware/EmRegsEZ.cpp` | DragonBall EZ variant |
| `Hardware/EmRegsVZ.cpp` | DragonBall VZ variant |
| `Hardware/EmSPISlaveADS784x.cpp` | Touchscreen ADC (returns 0 for pen) |

### ROM Patching
| File | Role |
|------|------|
| `Patches/EmPatchMgr.cpp` | Patch dispatcher, PuppetString |
| `Patches/EmPatchModuleSys.cpp` | System trap headpatches/tailpatches |
| `ROMStubs.cpp` | C++ wrappers for calling ROM functions |
| `EmSubroutine.cpp` | Parameter marshaling for ROM calls |

### ReControl & Debugging
| File | Role |
|------|------|
| `ReControl.cpp` | TCP server, session management, command dispatch |
| `ReControlCmds_Session.cpp` | state, reset, install, launch, save, load |
| `ReControlCmds_Input.cpp` | tap, tap-id, pen, key, type, button, menu, run |
| `ReControlCmds_Query.cpp` | screenshot, ui, peek, poke, regs, backtrace |
| `ReControlCmds_Debug.cpp` | break, watch, spy, log, gremlin, check |
| `ReControlCmds_Profile.cpp` | profile (if HAS_PROFILING) |

### Qt Platform
| File | Role |
|------|------|
| `platform/EmApplicationQt.cpp` | Qt app setup, HandleIdle timer |
| `platform/EmWindowQt.cpp` | Rendering, mouse/key input, skin |
| `platform/EmDlgQt.cpp` | Dialog system |

### Other Key Files
| File | Role |
|------|------|
| `DebugMgr.cpp` | GDB integration, breakpoints |
| `ErrorHandling.cpp` | Exception handling, error dialogs |
| `MetaMemory.cpp` | Memory tagging, access violation detection |
| `Profiling.cpp` | CPU cycle profiling (if HAS_PROFILING) |
| `EmScreen.cpp` | LCD framebuffer dirty tracking |

---

## Related Documentation

- `docs/stability-findings.md` — Six threading bugs found and fixed
- `docs/recontrol-protocol.md` — Complete ReControl TCP protocol reference
- `docs/debugging-infrastructure.md` — Debug feature catalog
- `docs/timer-accuracy-findings.md` — Timer emulation analysis
- `docs/qt-port-architectural-review.md` — Qt port design decisions
