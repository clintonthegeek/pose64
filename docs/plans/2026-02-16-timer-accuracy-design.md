# Timer Accuracy: Design Document

## Problem

POSE's hardware timer emulation uses a hardcoded constant (`increment = 4`)
to advance DragonBall timer counters once per CPU instruction. This has no
relationship to actual cycle costs. PalmOS reads its hardware timers to tell
time, so all time-dependent behaviour runs ~8x too fast. The existing speed
throttle correctly paces CPU cycles against wall-clock time, but PalmOS
ignores fCycleCount entirely.

See `docs/timer-accuracy.md` for the full analysis.

## Approach

Pass the actual CPU cycle cost of each instruction through the Cycle() call
chain. The timer advances by `cycles / prescaler` instead of a constant 4.
A Bresenham-style accumulator handles fractional ticks without division in
the hot path.

An "Accurate Timers" / "Legacy Timers" toggle lets users choose between
cycle-accurate timing and the original constant-increment behaviour.

## 1. Core Mechanism

### Signature change

```
EmHALHandler::Cycle(Bool sleeping)       -->  Cycle(Bool sleeping, int cycles)
EmHAL::Cycle(Bool sleeping)              -->  Cycle(Bool sleeping, int cycles)
```

### CYCLE macro

```cpp
#define CYCLE(sleeping, cycles)                          \
{                                                        \
    if (!session->IsNested()) {                          \
        EmHAL::Cycle(sleeping, cycles);                  \
        if (sleeping || ((++counter & 0x7FFF) == 0))     \
            this->CycleSlowly(sleeping);                 \
    }                                                    \
}
```

### Inner loop (EmCPU68K::Execute)

```cpp
int cycles = (functable[opcode])(opcode);
fCycleCount += cycles;
CYCLE(false, cycles);
```

### Stopped loop (sleeping)

```cpp
CYCLE(true, 0);   // no cycles to pass; sleeping path unchanged
```

## 2. Prescaler Handling

Timer clock source on all three DragonBall variants:

| Source       | Divisor | Bit shift |
|-------------|---------|-----------|
| Stop        | --      | (no advance) |
| System      | 1       | 0         |
| System/16   | 16      | 4         |
| 32.768 kHz  | special | slow path |
| TIN         | external| (not emulated) |

PalmOS always configures the tick timer to System/16. The shift value is
cached as a member (`fTimerShift`, `fTimerShiftMask`) when PalmOS writes
to `tmrControl`, using the existing register write-trap mechanism.

### Bresenham accumulator (no division in hot path)

```cpp
fTimerCycleAccum += cycles;
int ticks = fTimerCycleAccum >> fTimerShift;   // e.g. >> 4 for /16
fTimerCycleAccum &= fTimerShiftMask;           // e.g. & 0xF
if (ticks > 0) {
    WRITE_REGISTER(tmr1Counter, READ_REGISTER(tmr1Counter) + ticks);
    // ... compare check ...
}
```

For 32 kHz source (rare, not used by PalmOS for tick timer): fall back to
a slower `cycles * 32768 / clockFreq` calculation.

## 3. Accuracy Mode Toggle

A `bool fAccurateTimers` member on each EmRegsXX, cached from preference
`kPrefKeyTimerAccuracy` on session start and menu toggle.

In Cycle():

- **Accurate:** Use Bresenham accumulator with actual cycles.
- **Legacy:** Use constant 4, identical to current code.

The branch is perfectly predicted (never changes mid-execution), so the
cost is zero.

## 4. Speed Menu Layout

Timer mode and speed are both radio-button groups:

```
  (*) Accurate Timers
  ( ) Legacy Timers
  -----------------------
  ( ) Quarter Speed / 0.25x
  ( ) Half Speed / 0.5x
  (*) Real-time / 1x
  ( ) Double Speed / 2x
  ( ) Quad Speed / 4x
  ( ) 8x Speed / 8x
  ( ) Maximum
  -----------------------
  Manual...
```

Speed labels change based on timer mode:

- **Accurate:** "Quarter Speed", "Half Speed", "Real-time", etc.
  "Real-time" means actual Palm hardware speed.
- **Legacy:** "0.25x", "0.5x", "1x", "2x", etc.
  No "Real-time" label since the timers are disconnected from real time.

### Manual speed dialog

Two integer fields: numerator and denominator. User enters e.g. 1/32 for
1/32x speed. Computed as `numerator * 100 / denominator` to produce the
percentage value for `fEmulationSpeed`. Floor at 1%.

## 5. Sleeping Path

When the CPU is in STOP state, `CYCLE(true, 0)` is called. The sleeping
path skips the accumulator and jumps the counter directly past the compare
value to fire the interrupt immediately. This is correct: the real CPU is
idle waiting for the timer, so there's nothing to be cycle-accurate about.

No change from current behaviour.

## 6. Gremlins Time Cascade

The `fCycle/fTick/fSec/fMin/fHour` cascade:

- **Accurate mode:** `fCycle += ticks` (same prescaled value as timer).
- **Legacy mode:** `fCycle += 4` (unchanged).

## 7. Gremlins Warning

`StubAppGremlinsOn()` and `StubAppGremlinsOff()` (CGremlinsStubs.cpp) and
`Hordes::Initialize()` (Hordes.cpp) are currently stubbed out. When the
Gremlins UI is implemented, these entry points must warn the user if
Accurate Timers mode is enabled, since Gremlins expects to run at maximum
speed and cycle-accurate timers may cause PalmOS tick overflows or
unexpected timing behaviour.

TODO comments will be added at these locations during implementation.

## 8. Chip Variants

The change applies to all three Cycle() implementations:

| Variant    | File            | Timer(s)          | Notes |
|-----------|-----------------|-------------------|-------|
| EmRegs328 | EmRegs328.cpp   | tmr2 only         | Simple counter |
| EmRegsEZ  | EmRegsEZ.cpp    | tmr1 only         | Simple counter |
| EmRegsVZ  | EmRegsVZ.cpp    | tmr1 + tmr2       | tmr2 has software prescaler |

VZ Timer 2's existing `prescaleCounter` must also switch from constant
increment to actual cycles.

## Files Changed

- `EmHAL.h` -- Cycle() signature (virtual + static + inline)
- `EmCPU68K.cpp` -- CYCLE macro, Execute() inner loop, stopped loop
- `EmRegsVZ.cpp` -- Cycle() implementation, new members
- `EmRegsEZ.cpp` -- Cycle() implementation, new members
- `EmRegs328.cpp` -- Cycle() implementation, new members
- `EmRegsVZ.h` / `EmRegsEZ.h` / `EmRegs328.h` -- new member declarations
- `PreferenceMgr.h` -- kPrefKeyTimerAccuracy preference
- `EmMenus.cpp` -- speed submenu restructure
- `EmDlgQt.cpp` -- manual speed dialog
- `CGremlinsStubs.cpp` -- TODO warning comments
- `Hordes.cpp` -- TODO warning comments

## Verification

After implementation, the Welcome app animation on an m500 session should
loop 5 times in ~16.6 seconds at Real-time (1x) speed, matching a physical
Palm m500. At Quarter Speed, ~66 seconds.

Gremlins mode should continue to function in Legacy Timers mode.
