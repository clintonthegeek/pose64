# Timer Accuracy: The `increment = 4` Problem

## Summary

POSE's hardware timer emulation uses a hardcoded constant (`increment = 4`)
to advance the DragonBall timer counters once per CPU instruction.  This
value has no relationship to actual CPU cycle costs and produces timers
that fire at the wrong rate.  Animations, delays, and all PalmOS
time-dependent behaviour run far too fast compared to real hardware.

Measured on a physical Palm m500: the Welcome app animation loops 5 times
in 16.59 seconds.  In POSE64 at our slowest speed (0.25x, which should
take 66 seconds), the same animation completes in 3.19 seconds.

## Background: How PalmOS Tells Time

PalmOS does not use CPU cycle counting.  It relies on the DragonBall's
hardware timers, which are memory-mapped peripheral registers that count
up at a rate derived from the system clock:

    timer tick rate = system_clock / prescaler

PalmOS configures Timer 1 (EZ/VZ) or Timer 2 (328) so the compare-match
interrupt fires at 100 Hz (every 10 ms), giving the OS its "tick" for
`TimGetTicks()`, `SysTaskDelay()`, animation timing, and all scheduling.

The OS computes the timer compare value from the known clock frequency:

    compare = timer_tick_rate / 100

On a Palm m500 (DragonBall VZ at ~33 MHz, timer clocked by system/16):

    compare = 33,161,216 / 16 / 100 = 20,726

The timer must count from 0 to 20,726 in exactly 10 ms of real time.
That means the counter needs to advance by one for every 16 CPU clock
cycles.

## The Current Implementation

In `EmRegsVZ::Cycle()`, `EmRegsEZ::Cycle()`, and `EmRegs328::Cycle()`,
the timer counter is advanced like this:

```cpp
#define increment  4    // hardcoded constant, Release build
WRITE_REGISTER (tmr1Counter,
    READ_REGISTER (tmr1Counter) + (sleeping ? 1 : increment));
```

`Cycle()` is called **once per CPU instruction** from the inner emulation
loop (the `CYCLE` macro in `EmCPU68K.cpp`).  It does not receive the
cycle cost of the instruction that was just executed.

Meanwhile, the actual cycle cost is tracked separately:

```cpp
// In EmCPU68K::Execute, the inner loop:
fCycleCount += (functable[opcode]) (opcode);   // returns 4..20+ cycles
CYCLE (false);                                  // calls Cycle() with no args
```

The value returned by `functable[opcode]()` is the true MC68000 cycle
cost of the instruction (4, 8, 12, 16, 20, etc.).  This value goes into
`fCycleCount` and is never communicated to the timer.

## Why This Causes Incorrect Timing

### The math for the m500 (VZ, 33 MHz, timer prescaler /16)

PalmOS sets `tmr1Compare = 20,726`.

**On real hardware:**

The timer ticks once per 16 clock cycles.  To reach 20,726:

    20,726 ticks * 16 cycles/tick = 331,616 CPU cycles
    331,616 cycles / 33,161,216 Hz = 10.0 ms  (correct: 100 Hz)

**In the emulator:**

The timer advances by 4 per instruction.  To reach 20,726:

    20,726 / 4 = 5,182 instructions needed

Average MC68000 instruction cost is roughly 8 cycles (varies by workload;
register-to-register ALU ops cost 4, memory ops cost 8-20).  So:

    5,182 instructions * ~8 cycles/instruction = 41,453 CPU cycles
    41,453 cycles / 33,161,216 Hz = 1.25 ms

The timer fires every **1.25 ms** instead of every **10 ms**.  PalmOS
sees time passing **8x too fast**.

The exact ratio depends on the instruction mix.  For memory-heavy code
(common in PalmOS, which runs from Flash/ROM with wait states on real
hardware), the average is closer to 10-12 cycles, making the ratio worse.

### The speed throttle is correct but irrelevant

Our speed throttle in `EmCPU68K::CycleSlowly()` correctly paces
`fCycleCount` against wall-clock time.  Debug output confirms it:

```
[THROTTLE] speed=25 elapsed=57114546 clockFreq=33161216
           emulatedUs=1722329 targetWallUs=6889316
           actualWallUs=6889350 sleepUs=-34
```

The throttle makes the right number of CPU cycles happen per wall-clock
second.  But PalmOS ignores `fCycleCount` entirely — it reads its timer
registers to tell time, and those registers advance per-instruction at a
constant rate.  The throttle and the timers live in two disconnected
worlds.

## Historical Context: Why POSE Used a Constant

The original POSE (Palm OS Emulator, circa 1999-2003) was a debugging
tool, not a real-time emulator.  It ran the CPU loop as fast as the host
allowed with no throttle.  Animations, UI responsiveness, and timing were
not design goals.  The `increment = 4` was chosen empirically to make
PalmOS "roughly work" — not freeze, not assert, not spin.

The commented-out `PrvCalibrate()` function in `EmRegs328.cpp` shows the
original authors recognised the problem.  It attempted to dynamically
calibrate the increment by measuring wall-clock time:

```cpp
// Calibrate the value by which we increment the counter.
// The counter is set up so that it times out after 10 milliseconds
// so that it can increment the Palm OS's tick counter 100 times
// a second.  We would like tmrCounter to surpass tmrCompare
// after 10 milliseconds.  So figure out by how much we need to
// increment it in order for that to happen.
```

This was disabled with `#if 0` and the comment:

> Cycle is *very* sensitive to timing issue.  With this section of code,
> a Gremlins run can slow down by 5%.

The 5% Gremlins slowdown was unacceptable for POSE's use case (automated
testing at maximum speed).  Timing accuracy was traded for throughput.

The `_DEBUG` build uses `increment = 20` instead of 4, making the timer
fire 5x faster in debug builds.  This further confirms that the value was
never intended to be correct — just "good enough."

## What Needs to Change

The standard approach in retro emulation (NES, Game Boy, GBA, BBC Micro,
and every other accurately-timed emulator) is:

**The timer advances by the actual cycle cost of each instruction,
scaled by the timer's prescaler.**

### The core change

`Cycle()` must receive the number of CPU cycles consumed by the
instruction that was just executed.  The timer then advances by:

    cycles / prescaler_divisor

instead of a constant 4.

Concretely, the inner loop changes from:

```cpp
fCycleCount += (functable[opcode]) (opcode);
CYCLE (false);    // Cycle() ignores cycle cost
```

to something like:

```cpp
unsigned long cycles = (functable[opcode]) (opcode);
fCycleCount += cycles;
CYCLE (false, cycles);    // Cycle() uses actual cost
```

And inside `Cycle()`, the timer advance becomes:

```cpp
// Timer 1 clocked by system/16:
tmr1Counter += cycles / 16;
// with fractional cycle accumulation to avoid rounding loss
```

### What makes this delicate

1. **Performance.**  `Cycle()` is called for every single instruction
   in the hottest loop of the entire emulator.  The original authors
   obsessed over nanoseconds here (see the comments about 5% and 3%
   differences from register allocation and function inlining).  Adding
   a division is unacceptable in the inner loop.  The prescaler must be
   handled with bit shifts or lookup tables, not runtime division.

2. **Prescaler varies.**  The timer clock source is configured by PalmOS
   via the `tmrControl` register and can be:
   - System clock (no prescale)
   - System clock / 16
   - 32.768 kHz (CLK32)
   - TIN (external pin)
   - Stopped

   The prescaler divisor must be read from the register when it changes,
   not on every Cycle() call.  Cache it on write.

3. **Fractional cycles.**  If the prescaler is /16 and an instruction
   costs 10 cycles, the timer should advance by 0.625 ticks.  This
   requires an accumulator for the fractional remainder, similar to
   Bresenham's line algorithm.  Without it, rounding errors accumulate
   and drift the timer over time.

4. **Three chip variants.**  The change must be made consistently in
   `EmRegs328::Cycle()`, `EmRegsEZ::Cycle()`, and `EmRegsVZ::Cycle()`.
   Each has slightly different timer configurations (328 only has Timer 2
   for the system tick; EZ and VZ use Timer 1).

5. **Timer 2 on VZ has a software prescaler.**  The VZ `Cycle()` already
   implements a prescale counter for Timer 2:

   ```cpp
   static int prescaleCounter;
   if ((prescaleCounter -= increment) <= 0)
   {
       prescaleCounter = READ_REGISTER (tmr2Prescaler) * 1024;
       // ... advance timer 2 ...
   }
   ```

   This must also switch from constant `increment` to actual cycles.

6. **The RTC tick counter.**  All three Cycle() functions have an
   fCycle/fTick/fSec/fMin/fHour cascade that counts time based on
   `increment` and the timer compare value.  This is used for Gremlins
   time tracking and must also be converted to cycle-accurate counting.

7. **Sleeping mode.**  When the CPU is in STOP state (waiting for
   interrupt), Cycle() is called with `sleeping = true` and uses
   `increment = 1`.  The sleeping path needs its own treatment — it
   should advance the timer to the compare point immediately (since the
   CPU is doing nothing but waiting for the timer interrupt).

8. **Signature change propagation.**  `Cycle(Bool)` is a virtual method
   on `EmHALHandler`.  Adding a `cycles` parameter changes the virtual
   interface through `EmHAL::Cycle()`, `EmHALHandler::Cycle()`, and all
   three register implementations.  The `CYCLE` macro must also change.

## Verification

After the fix, the Welcome app animation on an m500 session should loop
5 times in approximately 16.6 seconds at Real-time (1x) speed — matching
the physical device.  At Quarter Speed (0.25x), it should take roughly
66 seconds.

The Gremlins automated test mode should continue to function (it runs at
maximum speed, where the throttle is disabled but the timers still need
to fire at a rate PalmOS can handle).

## References

- [Cycle Counting, Memory Stalls, Prefetch and Other Pitfalls (mGBA)](https://mgba.io/2015/06/27/cycle-counting-prefetch/)
- [How do the various emulators pace themselves? (stardot.org.uk)](https://stardot.org.uk/forums/viewtopic.php?t=21574)
- [Emulating The Core, Part 2: Interrupts and Timing (RealBoy)](https://realboyemulator.wordpress.com/2013/01/18/emulating-the-core-2/)
- [Game Boy Timer Emulation (codeslinger.co.uk)](http://www.codeslinger.co.uk/pages/projects/gameboy/timers.html)
