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

## Resolution (2026-02-16)

All of the above was implemented.  The inner loop now passes actual
cycle costs through `Cycle()`, which uses a Bresenham-style accumulator
with the prescaler encoded as a bit shift.  All three chip variants
(328/EZ/VZ) were updated consistently.  The STOP loop uses
`GetCyclesUntilNextInterrupt()` to sleep precisely until the next
timer compare match.

Three additional bugs were discovered and fixed during implementation.
The full implementation details and remaining limitations (UAE's ~2.7x
systematic cycle undercount, corrected by per-device benchmark
calibration) are documented in:

- `timer-accuracy-findings.md` — bugs found, diagnostic data, UAE cycle model analysis
- `benchmark-analysis-m500.md` — real hardware vs emulator benchmark comparison

## References

- [Cycle Counting, Memory Stalls, Prefetch and Other Pitfalls (mGBA)](https://mgba.io/2015/06/27/cycle-counting-prefetch/)
- [How do the various emulators pace themselves? (stardot.org.uk)](https://stardot.org.uk/forums/viewtopic.php?t=21574)
- [Emulating The Core, Part 2: Interrupts and Timing (RealBoy)](https://realboyemulator.wordpress.com/2013/01/18/emulating-the-core-2/)
- [Game Boy Timer Emulation (codeslinger.co.uk)](http://www.codeslinger.co.uk/pages/projects/gameboy/timers.html)
