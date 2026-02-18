# CPU Efficiency Improvements Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce host CPU usage from 100% to near-idle when the emulated Palm is waiting, and smooth out throttling when awake.

**Architecture:** Three independent improvements to `EmCPU68K.cpp` and the HAL layer: (1) sleep-until-interrupt in STOP mode, (2) remove diagnostic fprintf spam, (3) add minimum sleep floor to prevent sub-resolution spinning. All changes are in the timer-accuracy feature branch.

**Tech Stack:** C++, POSIX (gettimeofday, usleep), EmHAL virtual method chain

---

## Context

Palm OS spends most of its time in STOP mode (68000 STOP instruction) waiting for the next timer interrupt. Currently, `ExecuteStoppedLoop()` spins a tight loop advancing 16 cycles per iteration, calling `CycleSlowly()` every 4096 iterations. Even with throttling, this keeps one host CPU core pegged at 100%.

The fix: when we know the CPU is stopped and waiting for a timer interrupt, calculate exactly how many cycles until that interrupt fires, advance the timer by that amount, and `usleep()` for the corresponding wall-clock duration. This replaces thousands of spin iterations with a single sleep syscall.

### Key files

| File | Role |
|:---|:---|
| `src/core/Hardware/EmCPU68K.cpp` | Execute loop, stopped loop, CycleSlowly throttle |
| `src/core/Hardware/EmCPU68K.h` | Class definition |
| `src/core/Hardware/EmHAL.h` | HAL virtual method chain (add new method) |
| `src/core/Hardware/EmHAL.cpp` | HAL static wrappers |
| `src/core/Hardware/EmRegsVZ.cpp` | VZ timer Cycle(), timer registers |
| `src/core/Hardware/EmRegsVZ.h` | VZ class definition |
| `src/core/Hardware/EmRegsEZ.cpp` | EZ timer Cycle(), timer registers |
| `src/core/Hardware/EmRegsEZ.h` | EZ class definition |
| `src/core/Hardware/EmRegs328.cpp` | 328 timer Cycle(), timer registers |
| `src/core/Hardware/EmRegs328.h` | 328 class definition |
| `src/core/EmApplication.cpp` | Timer mode debug prints |

### Timer architecture recap

Each chip variant has a Bresenham-style timer accumulator in its `Cycle()` method:

```cpp
fTmr1CycleAccum += cycles;
int ticks = fTmr1CycleAccum >> fTmr1Shift;     // extract whole ticks
fTmr1CycleAccum &= fTmr1ShiftMask;             // keep remainder
counter += ticks;
if (counter > compare) { fire interrupt; reset counter; }
```

To calculate cycles until next interrupt:
```
ticksRemaining = compare - counter
cyclesRemaining = (ticksRemaining << shift) - cycleAccum
```

---

### Task 1: Remove diagnostic fprintf statements

All diagnostic `fprintf(stderr, ...)` calls added during timer accuracy development should be removed. They cause I/O stalls and produce noise.

**Files:**
- Modify: `src/core/Hardware/EmCPU68K.cpp:919-948` (TIMING DIAG block)
- Modify: `src/core/Hardware/EmRegsVZ.cpp:829-862` (TIMER1 CONFIG/RATE diagnostic)
- Modify: `src/core/Hardware/EmRegsVZ.cpp:2187-2209` (TIMER SHIFT diagnostic)
- Modify: `src/core/Hardware/EmRegsVZ.cpp:3281` (SetAccurateTimers debug)
- Modify: `src/core/Hardware/EmRegsEZ.cpp:675-706` (TIMER1 CONFIG/RATE diagnostic)
- Modify: `src/core/Hardware/EmRegsEZ.cpp:1804-1826` (TIMER SHIFT diagnostic)
- Modify: `src/core/Hardware/EmRegs328.cpp:794-825` (TIMER2 CONFIG/RATE diagnostic)
- Modify: `src/core/Hardware/EmRegs328.cpp:1961-1983` (TIMER SHIFT diagnostic)
- Modify: `src/core/EmApplication.cpp:1097,1106` (DoTimerMode debug)

**Step 1: Remove TIMING DIAG block from EmCPU68K.cpp**

In `CycleSlowly()` (lines 919-948), delete the entire diagnostic block:

```cpp
	// === DIAGNOSTIC: comprehensive timing analysis ===
	{
		static int diagCounter = 0;
		...
		}
	}
```

Keep the `int speed = ...` line above it and the `if (speed > 0)` block below.

**Step 2: Remove TIMER1 CONFIG/RATE diagnostic from EmRegsVZ.cpp**

In `Cycle()` (lines 829-862), delete the diagnostic block inside `if (counter > READ_REGISTER (tmr1Compare))`. The block starts with `// === DIAGNOSTIC:` and ends just before `WRITE_REGISTER (tmr1Status, ...)`. Also remove the `#include <sys/time.h>` if it was only added for diagnostics (check if gettimeofday is used elsewhere in the file).

**Step 3: Remove TIMER SHIFT diagnostics from EmRegsVZ.cpp**

Delete the two `fprintf` calls at lines 2187-2209 in `PrvUpdateTimerShift()`.

**Step 4: Remove SetAccurateTimers debug print from EmRegsVZ.cpp**

Delete the `fprintf` at line 3281.

**Step 5: Remove diagnostics from EmRegsEZ.cpp**

Same pattern as VZ: delete TIMER1 CONFIG/RATE block (lines 675-706) and TIMER SHIFT prints (lines 1804-1826). Remove `#include <sys/time.h>` if no longer needed.

**Step 6: Remove diagnostics from EmRegs328.cpp**

Same pattern: delete TIMER2 CONFIG/RATE block (lines 794-825) and TIMER SHIFT prints (lines 1961-1983). Remove `#include <sys/time.h>` if no longer needed.

**Step 7: Remove DoTimerMode debug prints from EmApplication.cpp**

Delete the `fprintf` calls at lines 1097 and 1106.

**Step 8: Build and verify**

```bash
make -C /home/clinton/dev/palmtest/QtPortPOSE/build -j$(($(nproc)-1))
```

Expected: clean build, no diagnostic output to stderr when running.

**Step 9: Commit**

```bash
git add src/core/Hardware/EmCPU68K.cpp src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.cpp src/core/Hardware/EmRegs328.cpp \
  src/core/EmApplication.cpp
git commit -m "Remove timer diagnostic fprintf statements"
```

---

### Task 2: Add GetCyclesUntilNextInterrupt to HAL

Add a virtual method to the HAL chain that each chip variant implements. Returns the number of CPU cycles until the next timer compare match.

**Files:**
- Modify: `src/core/Hardware/EmHAL.h:89,155`
- Modify: `src/core/Hardware/EmHAL.cpp`
- Modify: `src/core/Hardware/EmRegsVZ.h`
- Modify: `src/core/Hardware/EmRegsVZ.cpp`
- Modify: `src/core/Hardware/EmRegsEZ.h`
- Modify: `src/core/Hardware/EmRegsEZ.cpp`
- Modify: `src/core/Hardware/EmRegs328.h`
- Modify: `src/core/Hardware/EmRegs328.cpp`

**Step 1: Add virtual method to EmHALHandler**

In `src/core/Hardware/EmHAL.h`, after line 89 (`GetSleepCyclesPerTick`), add:

```cpp
		virtual int32			GetCyclesUntilNextInterrupt (void);
```

**Step 2: Add static wrapper to EmHAL**

In the same file, after line 155 (`GetSleepCyclesPerTick`), add:

```cpp
		static int32			GetCyclesUntilNextInterrupt (void);
```

**Step 3: Add default implementation in EmHAL.cpp**

Follow the pattern of other HAL methods. The base class returns a large default (meaning "no timer, don't know"):

```cpp
int32 EmHALHandler::GetCyclesUntilNextInterrupt (void)
{
	EmHALHandler* nextHandler = this->GetNextHandler ();
	if (nextHandler)
		return nextHandler->GetCyclesUntilNextInterrupt ();
	return 0x7FFFFFFF;
}

int32 EmHAL::GetCyclesUntilNextInterrupt (void)
{
	EmAssert (EmHAL::GetRootHandler ());
	return EmHAL::GetRootHandler ()->GetCyclesUntilNextInterrupt ();
}
```

**Step 4: Implement in EmRegsVZ**

Add declaration to `EmRegsVZ.h` in the public virtual overrides section:

```cpp
		virtual int32			GetCyclesUntilNextInterrupt (void);
```

Add implementation to `EmRegsVZ.cpp` (near `GetSleepCyclesPerTick`):

```cpp
int32 EmRegsVZ::GetCyclesUntilNextInterrupt (void)
{
	if (!fAccurateTimers)
		return 0x7FFFFFFF;

	// Timer 1
	if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlEnable) == 0)
		return 0x7FFFFFFF;

	uint16 counter = READ_REGISTER (tmr1Counter);
	uint16 compare = READ_REGISTER (tmr1Compare);

	if (counter >= compare)
		return 0;	// already past compare — interrupt imminent

	int32 ticksRemaining = (int32) compare - (int32) counter;
	int32 cyclesRemaining = (ticksRemaining << fTmr1Shift) - fTmr1CycleAccum;

	if (cyclesRemaining < 0)
		cyclesRemaining = 0;

	return cyclesRemaining;
}
```

**Step 5: Implement in EmRegsEZ**

Same pattern as VZ, using `fTmr1Shift`, `fTmr1CycleAccum`, `tmr1Counter`, `tmr1Compare`, and `hwrEZ328TmrControlEnable`.

**Step 6: Implement in EmRegs328**

Same pattern but using Timer 2: `fTmr2Shift`, `fTmr2CycleAccum`, `tmr2Counter`, `tmr2Compare`, and `hwr328TmrControlEnable`.

**Step 7: Build and verify**

```bash
make -C /home/clinton/dev/palmtest/QtPortPOSE/build -j$(($(nproc)-1))
```

**Step 8: Commit**

```bash
git add src/core/Hardware/EmHAL.h src/core/Hardware/EmHAL.cpp \
  src/core/Hardware/EmRegsVZ.h src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.h src/core/Hardware/EmRegsEZ.cpp \
  src/core/Hardware/EmRegs328.h src/core/Hardware/EmRegs328.cpp
git commit -m "Add GetCyclesUntilNextInterrupt to HAL chain"
```

---

### Task 3: Rewrite ExecuteStoppedLoop to sleep until next interrupt

Replace the spinning loop with a calculate-sleep-advance pattern.

**Files:**
- Modify: `src/core/Hardware/EmCPU68K.cpp:698-901` (ExecuteStoppedLoop)

**Step 1: Rewrite the stopped loop**

Replace the `do { ... } while (regs.spcflags & SPCFLAG_STOP)` loop body (lines 844-898) with:

```cpp
	do {
#if HAS_DEAD_MANS_SWITCH
		uint32 deadManNow = Platform::GetMilliseconds ();
		if ((deadManNow - deadManStart) > 5000)
		{
			Platform::Debugger ();
		}
#endif

		// Calculate how many cycles until the next timer interrupt fires.
		int32 cyclesToNext = EmHAL::GetCyclesUntilNextInterrupt ();

		// Clamp to a reasonable range:
		// - Minimum 16 cycles (don't advance by 0)
		// - Maximum ~65536 cycles (~2ms at 33MHz) to stay responsive
		if (cyclesToNext < 16)
			cyclesToNext = 16;
		if (cyclesToNext > 65536)
			cyclesToNext = 65536;

		// Advance timers by the calculated amount
		fCycleCount += cyclesToNext;
		EmHAL::Cycle (true, cyclesToNext);

		// Throttle: sleep for the corresponding wall-clock time
		int speed = fSession->fEmulationSpeed.load (std::memory_order_relaxed);
		if (speed > 0)
		{
			int32 clockFreq = EmHAL::GetSystemClockFrequency ();
			if (clockFreq > 0)
			{
				int64_t emulatedUs = (int64_t) cyclesToNext * 1000000LL / clockFreq;
				int64_t targetUs = emulatedUs * 100 / speed;

				if (targetUs > 200)		// only sleep if > 200us (below this, usleep is unreliable)
					usleep ((useconds_t) targetUs);
			}
		}

		// Perform expensive periodic tasks (LCD update, button polling, etc.)
		if ((++counter & 0x3F) == 0)
		{
			this->CycleSlowly (true);
		}

		// Process an interrupt (see if it's time to wake up).
		if (regs.spcflags & (SPCFLAG_INT | SPCFLAG_DOINT))
		{
			int32 interruptLevel = EmHAL::GetInterruptLevel ();

			regs.spcflags &= ~(SPCFLAG_INT | SPCFLAG_DOINT);

			if ((interruptLevel != -1) && (interruptLevel > regs.intmask))
			{
				this->ProcessInterrupt (interruptLevel);
				regs.stopped = 0;
				regs.spcflags &= ~SPCFLAG_STOP;
			}
		}

		if (this->CheckForBreak ())
		{
			return true;
		}
	} while (regs.spcflags & SPCFLAG_STOP);
```

**Key changes from the old loop:**
- Instead of fixed 16-cycle increments with 4096-iteration batches, advances by `cyclesToNext` (the exact gap to the next timer compare match)
- Sleeps for the wall-clock equivalent of that gap — one `usleep()` per iteration instead of spinning thousands of iterations
- CycleSlowly called every 64 iterations (0x3F mask) for LCD/button polling — less frequent because each iteration now represents much more emulated time
- Minimum sleep of 200us prevents sub-resolution spinning

**Step 2: Remove the old kSleepCyclesPerIter constant**

Delete lines 823-830 (the comment block and `const int kSleepCyclesPerIter = 16;`) — no longer needed.

**Step 3: Build and verify**

```bash
make -C /home/clinton/dev/palmtest/QtPortPOSE/build -j$(($(nproc)-1))
```

**Step 4: Test with m500 ROM**

Run the emulator. Expected behavior:
- Host CPU usage should drop dramatically when Palm is idle (showing launcher, no animation)
- Timer interrupts should still fire (Palm clock ticks, no freezes)
- Speed multipliers should still work correctly
- Gremlins should still function

**Step 5: Commit**

```bash
git add src/core/Hardware/EmCPU68K.cpp
git commit -m "STOP mode: sleep until next interrupt instead of spinning"
```

---

### Task 4: Add minimum sleep floor to awake-mode throttle

When the emulator is running (not stopped), the throttle in `CycleSlowly()` sometimes computes a `sleepUs` that's positive but too small for `usleep()` to honor (below OS timer resolution). Add a minimum floor.

**Files:**
- Modify: `src/core/Hardware/EmCPU68K.cpp:974-976` (usleep call in CycleSlowly)

**Step 1: Add minimum sleep floor**

Replace:
```cpp
				if (sleepUs > 0)
				{
					usleep ((useconds_t) sleepUs);
				}
```

With:
```cpp
				if (sleepUs > 500)
				{
					usleep ((useconds_t) sleepUs);
				}
				else if (sleepUs > 0)
				{
					// Sub-500us sleeps are unreliable on most systems.
					// Yield the CPU instead of busy-waiting.
					usleep (500);
				}
```

This ensures that when we're slightly ahead of schedule, we yield the CPU for at least 500us instead of spinning. The 500us floor is small enough to not affect emulation accuracy but large enough for the OS scheduler to actually deschedule us.

**Step 2: Build and verify**

```bash
make -C /home/clinton/dev/palmtest/QtPortPOSE/build -j$(($(nproc)-1))
```

**Step 3: Test**

Run with m500 ROM at 1x speed. Monitor CPU usage with `top` or `htop`. Expected:
- During animation/activity: CPU usage should be lower than before (no more sub-resolution spinning)
- During idle: CPU usage should be very low (from Task 3's STOP optimization)

**Step 4: Commit**

```bash
git add src/core/Hardware/EmCPU68K.cpp
git commit -m "Throttle: add minimum 500us sleep floor to prevent busy-waiting"
```

---

### Task 5: Verification across ROM images

Boot all three device types and confirm correct behavior.

**Tests:**
1. **m500 (VZ):** Boot to launcher, verify no freeze, check CPU usage is low when idle
2. **Palm V (EZ):** Same checks
3. **Palm III (328):** Same checks
4. **Speed menu:** Verify 0.25x, 0.50x, 1x, 2x all work
5. **Animations:** Welcome animation should still play smoothly
6. **Max speed (0x):** Should still run at full host speed with no artificial sleeping

**Expected outcome:** All ROMs boot, timers work, CPU usage drops from ~100% to <5% when idle.

**Step 1: Commit verification results as a final commit if needed**

```bash
git add -A
git commit -m "CPU efficiency: sleep-until-interrupt, remove diagnostics, throttle floor"
```

(Only if there are uncommitted fixes from testing.)
