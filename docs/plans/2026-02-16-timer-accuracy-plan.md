# Timer Accuracy Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace POSE's hardcoded `increment = 4` timer advancement with cycle-accurate timing, add an accuracy mode toggle, and restructure the speed menu.

**Architecture:** Pass actual CPU cycle costs through `Cycle()` to the timer hardware. A Bresenham accumulator divides by the timer prescaler using bit shifts. A runtime toggle switches between accurate and legacy (constant-4) paths.

**Tech Stack:** C++ (POSE codebase), Qt6 (for manual speed dialog)

---

### Task 1: Cycle() Signature Change (Plumbing)

Pass `cycles` through the entire call chain without changing any timer logic yet. All three Cycle() implementations receive the new parameter but ignore it, preserving exact current behaviour.

**Files:**
- Modify: `src/core/Hardware/EmHAL.h:51,109,159-163`
- Modify: `src/core/Hardware/EmCPU68K.cpp:125-144,476,520,864`
- Modify: `src/core/Hardware/EmRegsVZ.h:45`
- Modify: `src/core/Hardware/EmRegsVZ.cpp:800`
- Modify: `src/core/Hardware/EmRegsEZ.h:45`
- Modify: `src/core/Hardware/EmRegsEZ.cpp:648`
- Modify: `src/core/Hardware/EmRegs328.h:44`
- Modify: `src/core/Hardware/EmRegs328.cpp:814`

**Step 1: Change EmHALHandler virtual signature**

In `src/core/Hardware/EmHAL.h`, change line 51:

```cpp
// Before:
virtual void			Cycle					(Bool sleeping);
// After:
virtual void			Cycle					(Bool sleeping, int cycles);
```

Change the static method at line 109:

```cpp
// Before:
static void				Cycle					(Bool sleeping);
// After:
static void				Cycle					(Bool sleeping, int cycles);
```

Change the inline implementation at lines 159-163:

```cpp
// Before:
inline void EmHAL::Cycle (Bool sleeping)
{
	EmAssert (EmHAL::GetRootHandler());
	EmHAL::GetRootHandler()->Cycle (sleeping);
}
// After:
inline void EmHAL::Cycle (Bool sleeping, int cycles)
{
	EmAssert (EmHAL::GetRootHandler());
	EmHAL::GetRootHandler()->Cycle (sleeping, cycles);
}
```

**Step 2: Update the CYCLE macro**

In `src/core/Hardware/EmCPU68K.cpp`, change lines 125-144:

```cpp
// Before:
#define CYCLE(sleeping)															\
{																				\
	/* Don't do anything if we're in the middle of an ATrap call.  We don't */	\
	/* need interrupts firing or tmr counters incrementing. */					\
																				\
	EmAssert (session);															\
	if (!session->IsNested ())													\
	{																			\
		/* Perform CPU-specific idling. */										\
																				\
		EmHAL::Cycle (sleeping);												\
																				\
		/* Perform expensive operations. */										\
																				\
		if (sleeping || ((++counter & 0x7FFF) == 0))							\
		{																		\
			this->CycleSlowly (sleeping);										\
		}																		\
	}																			\
}

// After:
#define CYCLE(sleeping, cycles)													\
{																				\
	/* Don't do anything if we're in the middle of an ATrap call.  We don't */	\
	/* need interrupts firing or tmr counters incrementing. */					\
																				\
	EmAssert (session);															\
	if (!session->IsNested ())													\
	{																			\
		/* Perform CPU-specific idling. */										\
																				\
		EmHAL::Cycle (sleeping, cycles);										\
																				\
		/* Perform expensive operations. */										\
																				\
		if (sleeping || ((++counter & 0x7FFF) == 0))							\
		{																		\
			this->CycleSlowly (sleeping);										\
		}																		\
	}																			\
}
```

**Step 3: Update call sites in EmCPU68K::Execute**

At line 476, capture cycles into a local variable:

```cpp
// Before:
		fCycleCount += (functable[opcode]) (opcode);

		// ... (profiling code stays untouched) ...

		CYCLE (false);

// After:
		int cycles = (functable[opcode]) (opcode);
		fCycleCount += cycles;

		// ... (profiling code stays untouched) ...

		CYCLE (false, cycles);
```

Note: `functable[opcode]()` returns `unsigned long` on some paths. Casting to `int` is safe because m68k instruction cycle costs are always small positive values (4-160).

At line 864 (stopped loop):

```cpp
// Before:
		CYCLE (true);
// After:
		CYCLE (true, 0);
```

**Step 4: Update all three Cycle() implementations (signature only)**

In `src/core/Hardware/EmRegsVZ.h` line 45 and `EmRegsVZ.cpp` line 800:

```cpp
// Before:
virtual void			Cycle					(Bool sleeping);
void EmRegsVZ::Cycle (Bool sleeping)
// After:
virtual void			Cycle					(Bool sleeping, int cycles);
void EmRegsVZ::Cycle (Bool sleeping, int cycles)
```

Same change in `EmRegsEZ.h` line 45 / `EmRegsEZ.cpp` line 648, and `EmRegs328.h` line 44 / `EmRegs328.cpp` line 814.

The `cycles` parameter is received but not used yet. The function bodies are unchanged.

**Step 5: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile, zero warnings about unused `cycles` parameter (it's C++, unnamed params are fine, but keep the name for clarity).

Run the emulator, boot a session. Behaviour should be identical to before (the timer still uses constant 4).

**Step 6: Commit**

```bash
git add src/core/Hardware/EmHAL.h src/core/Hardware/EmCPU68K.cpp \
  src/core/Hardware/EmRegsVZ.h src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.h src/core/Hardware/EmRegsEZ.cpp \
  src/core/Hardware/EmRegs328.h src/core/Hardware/EmRegs328.cpp
git commit -m "Pass CPU cycle count through Cycle() call chain

Plumbing change only: Cycle(Bool) becomes Cycle(Bool, int cycles).
The cycles parameter is received but not yet used by timer logic.
All timer behaviour is identical to before."
```

---

### Task 2: Add Preference and Member Variables

Add `kPrefKeyTimerAccuracy` preference and `fAccurateTimers`, `fTmr1CycleAccum`, `fTmr1Shift`, `fTmr1ShiftMask` members to all three register classes. Initialize them. No behaviour change yet.

**Files:**
- Modify: `src/core/PreferenceMgr.h:412`
- Modify: `src/core/Hardware/EmRegsVZ.h:127-141`
- Modify: `src/core/Hardware/EmRegsVZ.cpp` (Initialize, Reset)
- Modify: `src/core/Hardware/EmRegsEZ.h:126-139`
- Modify: `src/core/Hardware/EmRegsEZ.cpp` (Initialize, Reset)
- Modify: `src/core/Hardware/EmRegs328.h:126-142`
- Modify: `src/core/Hardware/EmRegs328.cpp` (Initialize, Reset)

**Step 1: Add preference**

In `src/core/PreferenceMgr.h`, after the `EmulationSpeed` line (line 412), add:

```cpp
	DO_TO_PREF(TimerAccuracy,		long,				(1))						\
```

Value 1 = accurate (default), 0 = legacy.

**Step 2: Add member variables to EmRegsVZ**

In `src/core/Hardware/EmRegsVZ.h`, in the `protected:` section near line 127, add after `fCycle` (line 140):

```cpp
		bool					fAccurateTimers;
		int						fTmr1CycleAccum;
		int						fTmr1Shift;
		int						fTmr1ShiftMask;
		int						fTmr2CycleAccum;  // VZ has two timers
```

**Step 3: Initialize members in EmRegsVZ**

Find `EmRegsVZ::Initialize()` and `EmRegsVZ::Reset()`. In Reset (or Initialize, whichever zeroes state), add:

```cpp
	Preference<long> prefAccuracy (kPrefKeyTimerAccuracy);
	fAccurateTimers = (*prefAccuracy != 0);
	fTmr1CycleAccum = 0;
	fTmr1Shift = 4;          // default: system/16
	fTmr1ShiftMask = 0xF;    // (1 << 4) - 1
	fTmr2CycleAccum = 0;
```

**Step 4: Repeat for EmRegsEZ**

In `src/core/Hardware/EmRegsEZ.h`, add after `fCycle` (line 138):

```cpp
		bool					fAccurateTimers;
		int						fTmr1CycleAccum;
		int						fTmr1Shift;
		int						fTmr1ShiftMask;
```

Initialize in Reset():

```cpp
	Preference<long> prefAccuracy (kPrefKeyTimerAccuracy);
	fAccurateTimers = (*prefAccuracy != 0);
	fTmr1CycleAccum = 0;
	fTmr1Shift = 4;
	fTmr1ShiftMask = 0xF;
```

**Step 5: Repeat for EmRegs328**

In `src/core/Hardware/EmRegs328.h`, add after `fCycle` (line 141):

```cpp
		bool					fAccurateTimers;
		int						fTmr2CycleAccum;
		int						fTmr2Shift;
		int						fTmr2ShiftMask;
```

(328 uses tmr2, not tmr1.)

Initialize in Reset():

```cpp
	Preference<long> prefAccuracy (kPrefKeyTimerAccuracy);
	fAccurateTimers = (*prefAccuracy != 0);
	fTmr2CycleAccum = 0;
	fTmr2Shift = 4;
	fTmr2ShiftMask = 0xF;
```

**Step 6: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile. Members exist but are not read anywhere yet. Behaviour unchanged.

**Step 7: Commit**

```bash
git add src/core/PreferenceMgr.h \
  src/core/Hardware/EmRegsVZ.h src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.h src/core/Hardware/EmRegsEZ.cpp \
  src/core/Hardware/EmRegs328.h src/core/Hardware/EmRegs328.cpp
git commit -m "Add timer accuracy preference and accumulator members

New preference kPrefKeyTimerAccuracy (default: accurate).
Add fAccurateTimers, fTmrXCycleAccum, fTmrXShift, fTmrXShiftMask
members to all three register classes. Not yet used."
```

---

### Task 3: Cache Prescaler on tmrControl Write

Install custom write handlers for `tmr1Control` (VZ, EZ) and `tmr2Control` (VZ, 328) that parse the clock source bits and cache the shift value. Currently these registers use `StdWrite`; we add a thin wrapper.

**Files:**
- Modify: `src/core/Hardware/EmRegsVZ.h` (declare new handlers)
- Modify: `src/core/Hardware/EmRegsVZ.cpp` (implement handlers, install them)
- Modify: `src/core/Hardware/EmRegsEZ.h`
- Modify: `src/core/Hardware/EmRegsEZ.cpp`
- Modify: `src/core/Hardware/EmRegs328.h`
- Modify: `src/core/Hardware/EmRegs328.cpp`

**Step 1: Add write handler declaration to EmRegsVZ.h**

In the private section (near line 85), add:

```cpp
		void					tmr1ControlWrite		(emuptr address, int size, uint32 value);
		void					tmr2ControlWrite		(emuptr address, int size, uint32 value);
```

**Step 2: Implement the handlers in EmRegsVZ.cpp**

Add a helper to convert clock source bits to shift value, and the two write handlers:

```cpp
// Near the other write handlers (after tmr2StatusWrite)

static void PrvUpdateTimerShift (uint16 controlReg, int& shift, int& shiftMask)
{
	// Extract clock source bits [3:1] from tmrControl register.
	// All three DragonBall variants use the same bit layout.
	//
	// hwrVZ328TmrControlClkSrcStop     = 0x0000  -> timer stopped
	// hwrVZ328TmrControlClkSrcSys      = 0x0002  -> system clock (shift 0)
	// hwrVZ328TmrControlClkSrcSysBy16  = 0x0004  -> system/16 (shift 4)
	// hwrVZ328TmrControlClkSrcTIN      = 0x0006  -> external (treat as sys)
	// hwrVZ328TmrControlClkSrc32KHz    = 0x0008  -> 32 kHz (shift 0, special)

	uint16 clkSrc = controlReg & hwrVZ328TmrControlClkSrcMask;

	switch (clkSrc)
	{
		case hwrVZ328TmrControlClkSrcSysBy16:
			shift = 4;
			shiftMask = 0xF;
			break;
		case hwrVZ328TmrControlClkSrcSys:
		case hwrVZ328TmrControlClkSrcTIN:
		case hwrVZ328TmrControlClkSrc32KHz:
		default:
			shift = 0;
			shiftMask = 0;
			break;
	}
}

void EmRegsVZ::tmr1ControlWrite (emuptr address, int size, uint32 value)
{
	EmRegsVZ::StdWrite (address, size, value);
	PrvUpdateTimerShift (READ_REGISTER (tmr1Control), fTmr1Shift, fTmr1ShiftMask);
}

void EmRegsVZ::tmr2ControlWrite (emuptr address, int size, uint32 value)
{
	EmRegsVZ::StdWrite (address, size, value);

	// VZ Timer 2 shift is not directly used in the Bresenham path
	// because Timer 2 has its own software prescaler. But cache it
	// for consistency; the prescaleCounter logic handles Timer 2.
}
```

**Step 3: Install the handler in SetSubBankHandlers**

In `EmRegsVZ::SetSubBankHandlers()`, change line 676:

```cpp
// Before:
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr1Control);
// After:
	INSTALL_HANDLER (StdRead,			tmr1ControlWrite,		tmr1Control);
```

Change line 683:

```cpp
// Before:
	INSTALL_HANDLER (StdRead,			StdWrite,				tmr2Control);
// After:
	INSTALL_HANDLER (StdRead,			tmr2ControlWrite,		tmr2Control);
```

**Step 4: Repeat for EmRegsEZ**

Same pattern but EZ only has tmr1. Add to `EmRegsEZ.h`:

```cpp
		void					tmr1ControlWrite		(emuptr address, int size, uint32 value);
```

Implement in `EmRegsEZ.cpp`:

```cpp
void EmRegsEZ::tmr1ControlWrite (emuptr address, int size, uint32 value)
{
	EmRegsEZ::StdWrite (address, size, value);
	PrvUpdateTimerShift (READ_REGISTER (tmr1Control), fTmr1Shift, fTmr1ShiftMask);
}
```

Note: `PrvUpdateTimerShift` can be duplicated as a static function in each .cpp file (or shared via a common header). The EZ version uses `hwrEZ328TmrControlClkSrcMask` etc. — but the values are identical (0x000E) across all three variants, so the same function works. Use the variant-specific defines for correctness.

Install in `EmRegsEZ::SetSubBankHandlers()`.

**Step 5: Repeat for EmRegs328**

328 uses tmr2. Add `tmr2ControlWrite` to `EmRegs328.h` and `.cpp`. Install in `SetSubBankHandlers`.

**Step 6: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile. The shift values are now cached on every tmrControl write but not yet consumed. Boot a session to verify PalmOS still works.

**Step 7: Commit**

```bash
git add src/core/Hardware/EmRegsVZ.h src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.h src/core/Hardware/EmRegsEZ.cpp \
  src/core/Hardware/EmRegs328.h src/core/Hardware/EmRegs328.cpp
git commit -m "Cache timer prescaler shift on tmrControl write

Install custom write handlers for tmr1Control/tmr2Control that
parse the clock source bits and cache shift/mask values for use
by the Bresenham accumulator in the next task."
```

---

### Task 4: Implement Accurate Timer Path — EmRegsEZ (Simplest)

Start with EZ because it has only one timer and no prescaler complications. This is the first task that changes actual timer behaviour.

**Files:**
- Modify: `src/core/Hardware/EmRegsEZ.cpp:648-713`

**Step 1: Rewrite EmRegsEZ::Cycle()**

Replace the entire function body. The structure is: if accurate, use accumulator; else use legacy constant.

```cpp
void EmRegsEZ::Cycle (Bool sleeping, int cycles)
{
	// ===== Accurate timer path =====

	if (fAccurateTimers && !sleeping)
	{
		if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlEnable) != 0)
		{
			fTmr1CycleAccum += cycles;
			int ticks = fTmr1CycleAccum >> fTmr1Shift;
			fTmr1CycleAccum &= fTmr1ShiftMask;

			if (ticks > 0)
			{
				uint16 counter = READ_REGISTER (tmr1Counter) + ticks;
				WRITE_REGISTER (tmr1Counter, counter);

				if (counter > READ_REGISTER (tmr1Compare))
				{
					WRITE_REGISTER (tmr1Status, READ_REGISTER (tmr1Status) | hwrEZ328TmrStatusCompare);

					if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlFreeRun) == 0)
						WRITE_REGISTER (tmr1Counter, 0);

					if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlEnInterrupt) != 0)
					{
						WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrEZ328IntLoTimer);
						EmRegsEZ::UpdateInterrupts ();
					}
				}
			}
		}

		// Gremlins time cascade — use prescaled ticks
		int grTicks = cycles >> fTmr1Shift;
		if ((fCycle += grTicks) > READ_REGISTER (tmr1Compare))
		{
			fCycle = 0;
			if (++fTick >= 100) { fTick = 0;
				if (++fSec >= 60) { fSec = 0;
					if (++fMin >= 60) { fMin = 0;
						if (++fHour >= 24) { fHour = 0; }
					}
				}
			}
		}

		return;
	}

	// ===== Legacy path (unchanged original code) =====

	#define increment 4

	if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlEnable) != 0)
	{
		WRITE_REGISTER (tmr1Counter, READ_REGISTER (tmr1Counter) + (sleeping ? 1 : increment));

		if (sleeping || READ_REGISTER (tmr1Counter) > READ_REGISTER (tmr1Compare))
		{
			WRITE_REGISTER (tmr1Status, READ_REGISTER (tmr1Status) | hwrEZ328TmrStatusCompare);

			if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlFreeRun) == 0)
				WRITE_REGISTER (tmr1Counter, 0);

			if ((READ_REGISTER (tmr1Control) & hwrEZ328TmrControlEnInterrupt) != 0)
			{
				WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrEZ328IntLoTimer);
				EmRegsEZ::UpdateInterrupts ();
			}
		}
	}

	if ((fCycle += increment) > READ_REGISTER (tmr1Compare))
	{
		fCycle = 0;
		if (++fTick >= 100) { fTick = 0;
			if (++fSec >= 60) { fSec = 0;
				if (++fMin >= 60) { fMin = 0;
					if (++fHour >= 24) { fHour = 0; }
				}
			}
		}
	}

	#undef increment
}
```

Important: The `#define increment 4` / `#undef increment` pair replaces the old conditional `#if _DEBUG / #else / #endif` block. In legacy mode we always use 4 regardless of debug build. The debug build's `increment = 20` was never intended to be correct and the legacy path preserves the release behaviour.

**Step 2: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile.

Boot an EZ-based session (e.g. Palm IIIx). With the default preference (accurate=1), timer behaviour should now be cycle-accurate. Animations should visibly slow down to real-time-ish speed.

**Step 3: Commit**

```bash
git add src/core/Hardware/EmRegsEZ.cpp
git commit -m "Implement accurate timer path for EmRegsEZ

When fAccurateTimers is true, Timer 1 advances by actual CPU
cycles / prescaler using a Bresenham accumulator. Legacy path
preserved for Gremlins compatibility."
```

---

### Task 5: Implement Accurate Timer Path — EmRegs328

Same pattern as EZ but using tmr2 instead of tmr1.

**Files:**
- Modify: `src/core/Hardware/EmRegs328.cpp:814-888`

**Step 1: Rewrite EmRegs328::Cycle()**

Same structure as EZ, but replace all `tmr1` references with `tmr2`, and use `fTmr2CycleAccum` / `fTmr2Shift` / `fTmr2ShiftMask`. Also uses `hwr328*` defines instead of `hwrEZ328*`.

```cpp
void EmRegs328::Cycle (Bool sleeping, int cycles)
{
	// ===== Accurate timer path =====

	if (fAccurateTimers && !sleeping)
	{
		if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnable) != 0)
		{
			fTmr2CycleAccum += cycles;
			int ticks = fTmr2CycleAccum >> fTmr2Shift;
			fTmr2CycleAccum &= fTmr2ShiftMask;

			if (ticks > 0)
			{
				uint16 counter = READ_REGISTER (tmr2Counter) + ticks;
				WRITE_REGISTER (tmr2Counter, counter);

				if (counter > READ_REGISTER (tmr2Compare))
				{
					WRITE_REGISTER (tmr2Status, READ_REGISTER (tmr2Status) | hwr328TmrStatusCompare);

					if ((READ_REGISTER (tmr2Control) & hwr328TmrControlFreeRun) == 0)
						WRITE_REGISTER (tmr2Counter, 0);

					if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnInterrupt) != 0)
					{
						WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwr328IntLoTimer2);
						EmRegs328::UpdateInterrupts ();
					}
				}
			}
		}

		int grTicks = cycles >> fTmr2Shift;
		if ((fCycle += grTicks) > READ_REGISTER (tmr2Compare))
		{
			fCycle = 0;
			if (++fTick >= 100) { fTick = 0;
				if (++fSec >= 60) { fSec = 0;
					if (++fMin >= 60) { fMin = 0;
						if (++fHour >= 24) { fHour = 0; }
					}
				}
			}
		}

		return;
	}

	// ===== Legacy path (unchanged original code) =====

	#define increment 4

	if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnable) != 0)
	{
		WRITE_REGISTER (tmr2Counter, READ_REGISTER (tmr2Counter) + (sleeping ? 1 : increment));

		if (sleeping || READ_REGISTER (tmr2Counter) > READ_REGISTER (tmr2Compare))
		{
			WRITE_REGISTER (tmr2Status, READ_REGISTER (tmr2Status) | hwr328TmrStatusCompare);

			if ((READ_REGISTER (tmr2Control) & hwr328TmrControlFreeRun) == 0)
				WRITE_REGISTER (tmr2Counter, 0);

			if ((READ_REGISTER (tmr2Control) & hwr328TmrControlEnInterrupt) != 0)
			{
				WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwr328IntLoTimer2);
				EmRegs328::UpdateInterrupts ();
			}
		}
	}

	if ((fCycle += increment) > READ_REGISTER (tmr2Compare))
	{
		fCycle = 0;
		if (++fTick >= 100) { fTick = 0;
			if (++fSec >= 60) { fSec = 0;
				if (++fMin >= 60) { fMin = 0;
					if (++fHour >= 24) { fHour = 0; }
				}
			}
		}
	}

	#undef increment
}
```

Also remove the disabled `PrvCalibrate()` function and its associated `#if 0` block (lines 767-829) — it's dead code that the new accurate path supersedes.

**Step 2: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile. Boot a 328-based session to verify.

**Step 3: Commit**

```bash
git add src/core/Hardware/EmRegs328.cpp
git commit -m "Implement accurate timer path for EmRegs328

Same Bresenham accumulator pattern as EZ, using tmr2.
Remove dead PrvCalibrate() code."
```

---

### Task 6: Implement Accurate Timer Path — EmRegsVZ (Most Complex)

VZ has two timers: Timer 1 (simple, used for OS tick) and Timer 2 (has software prescaler). Both need accurate paths.

**Files:**
- Modify: `src/core/Hardware/EmRegsVZ.cpp:800-918`

**Step 1: Rewrite EmRegsVZ::Cycle()**

Timer 1 uses the same Bresenham pattern as EZ. Timer 2 is more complex because of the existing `prescaleCounter` mechanism — in accurate mode, the prescale counter should decrement by actual cycles instead of the constant increment.

```cpp
void EmRegsVZ::Cycle (Bool sleeping, int cycles)
{
	// ===== Accurate timer path =====

	if (fAccurateTimers && !sleeping)
	{
		// --- Timer 1 ---
		if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlEnable) != 0)
		{
			fTmr1CycleAccum += cycles;
			int ticks = fTmr1CycleAccum >> fTmr1Shift;
			fTmr1CycleAccum &= fTmr1ShiftMask;

			if (ticks > 0)
			{
				uint16 counter = READ_REGISTER (tmr1Counter) + ticks;
				WRITE_REGISTER (tmr1Counter, counter);

				if (counter > READ_REGISTER (tmr1Compare))
				{
					WRITE_REGISTER (tmr1Status, READ_REGISTER (tmr1Status) | hwrVZ328TmrStatusCompare);

					if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlFreeRun) == 0)
						WRITE_REGISTER (tmr1Counter, 0);

					if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlEnInterrupt) != 0)
					{
						WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrVZ328IntLoTimer);
						EmRegsVZ::UpdateInterrupts ();
					}
				}
			}
		}

		// --- Timer 2 (with software prescaler) ---
		if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlEnable) != 0)
		{
			static int prescaleCounter;

			if ((prescaleCounter -= cycles) <= 0)
			{
				prescaleCounter = READ_REGISTER (tmr2Prescaler) * 1024;

				// Timer 2 counter uses its own accumulator
				fTmr2CycleAccum += cycles;
				int t2ticks = fTmr2CycleAccum >> fTmr1Shift;
				fTmr2CycleAccum &= fTmr1ShiftMask;

				if (t2ticks > 0)
				{
					uint16 counter = READ_REGISTER (tmr2Counter) + t2ticks;
					WRITE_REGISTER (tmr2Counter, counter);

					if (counter > READ_REGISTER (tmr2Compare))
					{
						WRITE_REGISTER (tmr2Status, READ_REGISTER (tmr2Status) | hwrVZ328TmrStatusCompare);

						if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlFreeRun) == 0)
							WRITE_REGISTER (tmr2Counter, 0);

						if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlEnInterrupt) != 0)
						{
							WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrVZ328IntLoTimer2);
							EmRegsVZ::UpdateInterrupts ();
						}
					}
				}
			}
		}

		// Gremlins time cascade
		int grTicks = cycles >> fTmr1Shift;
		if ((fCycle += grTicks) > READ_REGISTER (tmr1Compare))
		{
			fCycle = 0;
			if (++fTick >= 100) { fTick = 0;
				if (++fSec >= 60) { fSec = 0;
					if (++fMin >= 60) { fMin = 0;
						if (++fHour >= 24) { fHour = 0; }
					}
				}
			}
		}

		return;
	}

	// ===== Legacy path (unchanged original code) =====

	#define increment 4

	if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlEnable) != 0)
	{
		WRITE_REGISTER (tmr1Counter, READ_REGISTER (tmr1Counter) + (sleeping ? 1 : increment));

		if (sleeping || READ_REGISTER (tmr1Counter) > READ_REGISTER (tmr1Compare))
		{
			WRITE_REGISTER (tmr1Status, READ_REGISTER (tmr1Status) | hwrVZ328TmrStatusCompare);

			if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlFreeRun) == 0)
				WRITE_REGISTER (tmr1Counter, 0);

			if ((READ_REGISTER (tmr1Control) & hwrVZ328TmrControlEnInterrupt) != 0)
			{
				WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrVZ328IntLoTimer);
				EmRegsVZ::UpdateInterrupts ();
			}
		}
	}

#if 1
	if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlEnable) != 0)
	{
		static int prescaleCounter;

		if ((prescaleCounter -= (sleeping ? (increment * 1024) : increment)) <= 0)
		{
			prescaleCounter = READ_REGISTER (tmr2Prescaler) * 1024;

			WRITE_REGISTER (tmr2Counter, READ_REGISTER (tmr2Counter) + (sleeping ? 1 : increment));

			if (sleeping || READ_REGISTER (tmr2Counter) > READ_REGISTER (tmr2Compare))
			{
				WRITE_REGISTER (tmr2Status, READ_REGISTER (tmr2Status) | hwrVZ328TmrStatusCompare);

				if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlFreeRun) == 0)
					WRITE_REGISTER (tmr2Counter, 0);

				if ((READ_REGISTER (tmr2Control) & hwrVZ328TmrControlEnInterrupt) != 0)
				{
					WRITE_REGISTER (intPendingLo, READ_REGISTER (intPendingLo) | hwrVZ328IntLoTimer2);
					EmRegsVZ::UpdateInterrupts ();
				}
			}
		}
	}
#endif

	if ((fCycle += increment) > READ_REGISTER (tmr1Compare))
	{
		fCycle = 0;
		if (++fTick >= 100) { fTick = 0;
			if (++fSec >= 60) { fSec = 0;
				if (++fMin >= 60) { fMin = 0;
					if (++fHour >= 24) { fHour = 0; }
				}
			}
		}
	}

	#undef increment
}
```

**Step 2: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile.

**This is the critical test.** Boot an m500 session (VZ chip). Navigate to the Welcome app. At the default 1x speed with accurate timers, the animation should loop 5 times in approximately 16.6 seconds. Time it with a stopwatch.

If it's close (within ~1 second), the implementation is correct. If it's still too fast, check that fTmr1Shift is being set to 4 (add a debug printf in `tmr1ControlWrite` to verify).

**Step 3: Commit**

```bash
git add src/core/Hardware/EmRegsVZ.cpp
git commit -m "Implement accurate timer path for EmRegsVZ

Timer 1: Bresenham accumulator with prescaler shift.
Timer 2: prescaleCounter decrements by actual cycles.
Legacy path preserved unchanged for Gremlins mode."
```

---

### Task 7: Gremlins TODO Comments

Add TODO warnings at Gremlins entry points for future implementers.

**Files:**
- Modify: `src/core/CGremlinsStubs.cpp:239-242`
- Modify: `src/core/Hordes.cpp:91-94`

**Step 1: Add TODO to StubAppGremlinsOn()**

In `src/core/CGremlinsStubs.cpp`, in `StubAppGremlinsOn()` (line 239):

```cpp
void StubAppGremlinsOn (void)
{
	// Called from Gremlins::Initialize.

	// TODO: When Gremlins UI is implemented, warn the user here if
	// Accurate Timers mode is enabled.  Gremlins runs at maximum speed
	// and cycle-accurate timers may cause PalmOS tick overflows or
	// unexpected timing behaviour.  Consider auto-switching to Legacy
	// Timers mode for Gremlins runs.
}
```

**Step 2: Add TODO to Hordes::Initialize()**

In `src/core/Hordes.cpp`, in `Hordes::Initialize()` (line 91):

```cpp
void Hordes::Initialize (void)
{
	// TODO: When Gremlins UI is implemented, check kPrefKeyTimerAccuracy
	// here and warn if accurate timers are enabled.  Gremlins expects
	// maximum-speed execution; cycle-accurate timers may cause issues.

	gTheGremlin.Reset ();
}
```

**Step 3: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile (comments only).

**Step 4: Commit**

```bash
git add src/core/CGremlinsStubs.cpp src/core/Hordes.cpp
git commit -m "Add Gremlins TODO warnings for timer accuracy mode"
```

---

### Task 8: Speed Menu Restructure

Rebuild the speed submenu with two radio-button groups (timer mode + speed), sorted slowest-to-fastest, with dynamic labels. Add new commands for timer mode and manual speed.

**Files:**
- Modify: `src/core/EmCommands.h:76-84`
- Modify: `src/core/Strings.r.h:200-208`
- Modify: `src/platform/ResStrings.cpp:222-230`
- Modify: `src/core/EmMenus.cpp:252-258,604-612,950-958,1003-1031`
- Modify: `src/core/EmApplication.cpp:108,1050-1069`
- Modify: `src/core/EmSession.h` (add fAccurateTimers atomic)
- Modify: `src/core/EmSession.cpp` (load preference on session start)

**Step 1: Add new command IDs**

In `src/core/EmCommands.h`, add before/after the existing speed commands:

```cpp
	kCommandTimerAccurate,
	kCommandTimerLegacy,

	kCommandSpeedQuarter,
	kCommandSpeedHalf,
	kCommandSpeed1x,
	kCommandSpeed2x,
	kCommandSpeed4x,
	kCommandSpeed8x,
	kCommandSpeedMax,

	kCommandSpeedManual,
```

**Step 2: Add string IDs**

In `src/core/Strings.r.h`, add:

```cpp
#define kStr_MenuTimerAccurate			1460
#define kStr_MenuTimerLegacy			1461
#define kStr_MenuSpeedManual			1462
```

**Step 3: Add string values**

In `src/platform/ResStrings.cpp`, add:

```cpp
	{ kStr_MenuTimerAccurate, "&Accurate Timers" },
	{ kStr_MenuTimerLegacy, "&Legacy Timers" },
	{ kStr_MenuSpeedManual, "Ma&nual..." },
```

Also update the speed labels. These labels are static; the dynamic label switching (accurate vs legacy) is handled by `PrvGetItemText()` or by rebuilding the menu on timer mode change. The simpler approach: store the accurate-mode labels as the defaults, and override them at display time if in legacy mode. The exact approach depends on how `EmMenuItem::GetTitle()` works — check whether it supports dynamic text.

Alternatively (simpler): keep static labels and just use the accurate-mode labels. The menu always shows "Quarter Speed", "Real-time", etc. The radio button for "Accurate/Legacy" provides context. This avoids dynamic label complexity. The user knows that "Real-time" is meaningful in accurate mode and approximate in legacy mode.

**Decision: Use static labels.** Dynamic labels add complexity for minimal benefit. The radio button group makes the mode clear.

Update existing speed labels to remove keyboard accelerator markers that conflict, and reorder:

```cpp
	{ kStr_MenuSpeedQuarter, "&Quarter Speed" },
	{ kStr_MenuSpeedHalf, "&Half Speed" },
	{ kStr_MenuSpeed1x, "&Real-time" },
	{ kStr_MenuSpeed2x, "&Double Speed" },
	{ kStr_MenuSpeed4x, "Q&uad Speed" },
	{ kStr_MenuSpeed8x, "&8x Speed" },
	{ kStr_MenuSpeedMax, "&Maximum" },
```

**Step 4: Build the submenu**

In `src/core/EmMenus.cpp`, in the menu construction function, replace the subMenuSpeed construction (lines 604-612):

```cpp
	EmMenuItemList	subMenuSpeed;
	::PrvAddMenuItem (subMenuSpeed, kCommandTimerAccurate);
	::PrvAddMenuItem (subMenuSpeed, kCommandTimerLegacy);
	::PrvAddMenuItem (subMenuSpeed, __________);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeedQuarter);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeedHalf);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeed1x);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeed2x);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeed4x);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeed8x);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeedMax);
	::PrvAddMenuItem (subMenuSpeed, __________);
	::PrvAddMenuItem (subMenuSpeed, kCommandSpeedManual);
```

**Step 5: Add kPrvMenuItems entries**

In the `kPrvMenuItems` array, add entries for the new commands:

```cpp
	{ kCommandTimerAccurate,	kStr_MenuTimerAccurate },
	{ kCommandTimerLegacy,		kStr_MenuTimerLegacy },
	{ kCommandSpeedManual,		kStr_MenuSpeedManual },
```

**Step 6: Update PrvGetItemEnabled**

Add to the enabled check (around line 950):

```cpp
		case kCommandTimerAccurate:		return true;
		case kCommandTimerLegacy:		return true;
		case kCommandSpeedManual:		return true;
```

**Step 7: Update PrvGetItemChecked**

Expand the checked logic (around line 1003):

```cpp
Bool PrvGetItemChecked (const EmMenuItem& item)
{
	EmCommandID	id = item.GetCommand ();

	// Timer mode radio buttons
	if (id == kCommandTimerAccurate || id == kCommandTimerLegacy)
	{
		Preference<long> prefAccuracy (kPrefKeyTimerAccuracy);
		long accuracy = *prefAccuracy;

		if (id == kCommandTimerAccurate)	return accuracy != 0;
		if (id == kCommandTimerLegacy)		return accuracy == 0;
	}

	// Speed radio buttons
	if (id < kCommandSpeedQuarter || id > kCommandSpeedMax)
		return false;

	Preference<long> prefSpeed (kPrefKeyEmulationSpeed);
	long speed = *prefSpeed;

	switch (id)
	{
		case kCommandSpeedQuarter:	return speed == 25;
		case kCommandSpeedHalf:		return speed == 50;
		case kCommandSpeed1x:		return speed == 100;
		case kCommandSpeed2x:		return speed == 200;
		case kCommandSpeed4x:		return speed == 400;
		case kCommandSpeed8x:		return speed == 800;
		case kCommandSpeedMax:		return speed != 25 && speed != 50
										&& speed != 100 && speed != 200
										&& speed != 400 && speed != 800;
		default: break;
	}

	return false;
}
```

**Step 8: Handle timer mode commands in EmApplication**

In `src/core/EmApplication.cpp`, add handler for `kCommandTimerAccurate` and `kCommandTimerLegacy`. Add a dispatch entry in the command table (around line 108):

```cpp
	{ kCommandTimerAccurate,	&EmApplication::DoTimerMode,		0	},
	{ kCommandTimerLegacy,		&EmApplication::DoTimerMode,		0	},
	{ kCommandSpeedManual,		&EmApplication::DoSpeedManual,		0	},
```

Implement `DoTimerMode`:

```cpp
void EmApplication::DoTimerMode (EmCommandID cmd)
{
	long accuracy = (cmd == kCommandTimerAccurate) ? 1 : 0;
	Preference<long> p (kPrefKeyTimerAccuracy);
	p = accuracy;

	// Notify the running session to update its cached fAccurateTimers
	if (gSession)
		gSession->UpdateTimerAccuracy (accuracy != 0);
}
```

This requires an `UpdateTimerAccuracy()` method on EmSession that propagates to the current EmRegsXX handler. The simplest approach: add a method to EmHALHandler and EmHAL that sets `fAccurateTimers`, then call it from EmSession.

Add to `EmHAL.h`:

```cpp
// In EmHALHandler:
virtual void			SetAccurateTimers		(bool accurate);

// In EmHAL:
static void				SetAccurateTimers		(bool accurate);
```

Implement in `EmHAL.cpp`:

```cpp
void EmHAL::SetAccurateTimers (bool accurate)
{
	EmAssert (EmHAL::GetRootHandler());
	EmHAL::GetRootHandler()->SetAccurateTimers (accurate);
}
```

Default implementation in `EmHALHandler` (base class, in `EmHAL.cpp`):

```cpp
void EmHALHandler::SetAccurateTimers (bool) {}
```

Override in each EmRegsXX:

```cpp
void EmRegsVZ::SetAccurateTimers (bool accurate)
{
	fAccurateTimers = accurate;
	fTmr1CycleAccum = 0;
	fTmr2CycleAccum = 0;
}
```

(Same pattern for EZ and 328.)

Then `EmSession::UpdateTimerAccuracy()` just calls `EmHAL::SetAccurateTimers(accurate)`.

**Step 9: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile.

Boot a session. Right-click for popup menu. The Speed submenu should show the new layout with timer mode radio buttons and sorted speed list. Switching between Accurate and Legacy should visibly change timer behaviour.

**Step 10: Commit**

```bash
git add src/core/EmCommands.h src/core/Strings.r.h \
  src/platform/ResStrings.cpp src/core/EmMenus.cpp \
  src/core/EmApplication.h src/core/EmApplication.cpp \
  src/core/Hardware/EmHAL.h src/core/Hardware/EmHAL.cpp \
  src/core/Hardware/EmRegsVZ.h src/core/Hardware/EmRegsVZ.cpp \
  src/core/Hardware/EmRegsEZ.h src/core/Hardware/EmRegsEZ.cpp \
  src/core/Hardware/EmRegs328.h src/core/Hardware/EmRegs328.cpp \
  src/core/EmSession.h src/core/EmSession.cpp
git commit -m "Restructure speed menu with timer mode toggle

Add Accurate/Legacy timer radio buttons, reorder speed presets
slowest-to-fastest, add Manual... placeholder. Timer mode switch
propagates to running session via EmHAL::SetAccurateTimers()."
```

---

### Task 9: Manual Speed Dialog

Add the "Manual..." menu item handler that opens a dialog with numerator/denominator fields.

**Files:**
- Modify: `src/core/EmApplication.cpp` (DoSpeedManual handler)
- Modify: `src/core/EmDlg.h` (declare dialog)
- Modify: `src/core/EmDlg.cpp` (or `src/platform/EmDlgQt.cpp`)

**Step 1: Determine dialog approach**

Check how existing dialogs work in this codebase. The `EmDlg` class has static methods like `DoGetFile()`, `DoAboutBox()`, etc. Qt-specific implementations are in `EmDlgQt.cpp`. We'll add a `DoManualSpeed()` method.

Add to `EmDlg.h`:

```cpp
static EmDlgItemID		DoManualSpeed		(int& numerator, int& denominator);
```

**Step 2: Implement in EmDlgQt.cpp**

This is Qt-specific. Use a simple `QDialog` with two `QSpinBox` widgets:

```cpp
EmDlgItemID EmDlg::DoManualSpeed (int& numerator, int& denominator)
{
	QDialog dlg;
	dlg.setWindowTitle ("Manual Emulation Speed");

	QVBoxLayout* layout = new QVBoxLayout (&dlg);

	QLabel* label = new QLabel ("Enter speed as a fraction of real-time:");
	layout->addWidget (label);

	QHBoxLayout* fracLayout = new QHBoxLayout;
	QSpinBox* numBox = new QSpinBox;
	numBox->setRange (1, 1000);
	numBox->setValue (numerator);
	QLabel* slash = new QLabel ("/");
	QSpinBox* denBox = new QSpinBox;
	denBox->setRange (1, 1000);
	denBox->setValue (denominator);
	fracLayout->addWidget (numBox);
	fracLayout->addWidget (slash);
	fracLayout->addWidget (denBox);
	layout->addLayout (fracLayout);

	QDialogButtonBox* buttons = new QDialogButtonBox (
		QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	layout->addWidget (buttons);

	QObject::connect (buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect (buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	if (dlg.exec () == QDialog::Accepted)
	{
		numerator = numBox->value ();
		denominator = denBox->value ();
		return kDlgItemOK;
	}

	return kDlgItemCancel;
}
```

**Step 3: Implement DoSpeedManual in EmApplication.cpp**

```cpp
void EmApplication::DoSpeedManual (EmCommandID)
{
	int numerator = 1;
	int denominator = 1;

	if (EmDlg::DoManualSpeed (numerator, denominator) == kDlgItemOK)
	{
		// Compute speed as percentage: (num / den) * 100
		// Floor at 1% to prevent divide-by-zero behaviour
		long speed = (long) numerator * 100 / denominator;
		if (speed < 1) speed = 1;

		Preference<long> p (kPrefKeyEmulationSpeed);
		p = speed;

		if (gSession)
			gSession->fEmulationSpeed.store ((int) speed, std::memory_order_relaxed);
	}
}
```

**Step 4: Build and verify**

Run: `cd /home/clinton/dev/palmtest/QtPortPOSE && ./build.sh`
Expected: Clean compile.

Boot a session. Open Speed menu, click "Manual...". Enter 1/32. Verify emulation slows dramatically. Enter 1/1. Verify it returns to real-time. Enter 2/1. Verify it speeds up.

**Step 5: Commit**

```bash
git add src/core/EmDlg.h src/platform/EmDlgQt.cpp \
  src/core/EmApplication.h src/core/EmApplication.cpp
git commit -m "Add Manual Speed dialog with fraction input

Numerator/denominator entry computes arbitrary emulation speed
as a percentage of real-time. Supports very slow speeds (1/32x)
for debugging and observation."
```

---

### Task 10: Final Verification

End-to-end testing across all three chip variants.

**Step 1: Test VZ (m500)**

- Boot m500 session
- Verify Accurate Timers is selected (default)
- Set speed to Real-time
- Open Welcome app, time the animation: should be ~16.6s for 5 loops
- Switch to Legacy Timers: animation should speed up dramatically
- Switch back to Accurate, set Quarter Speed: ~66s for 5 loops
- Test Manual: enter 1/16, verify very slow animation

**Step 2: Test EZ (e.g. Palm V / IIIx)**

- Boot an EZ-based session
- Same timing test — verify animations run at expected speed
- Toggle Accurate/Legacy, verify the switch works

**Step 3: Test 328 (e.g. Pilot, Palm III)**

- Boot a 328-based session
- Same verification

**Step 4: Test sleeping path**

- Let the device auto-off (screen dims, enters STOP state)
- Tap to wake up — should wake instantly, not hang
- Verify this works in both Accurate and Legacy modes

**Step 5: Test Maximum speed**

- Set speed to Maximum in both Accurate and Legacy modes
- Verify the emulator runs fast and doesn't freeze
- In Accurate mode at Maximum, timers will fire very rapidly but PalmOS should still function

**Step 6: Commit (if any fixes were needed)**

If tests reveal issues, fix and commit. Otherwise, this is a verification-only step.
