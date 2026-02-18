# Timer Accuracy: Implementation Findings

## What We Set Out To Do

Replace POSE's hardcoded `increment = 4` timer advancement with cycle-accurate
timer counting.  The original `timer-accuracy.md` document describes the
problem: PalmOS reads hardware timer registers to tell time, but POSE advanced
those registers by a constant 4 per instruction regardless of actual cycle
cost.  All time-dependent behaviour ran ~8x too fast.

The fix: pass actual UAE instruction cycle costs through `Cycle()` and use a
Bresenham-style accumulator to advance timer counters at the correct rate
relative to the system clock and prescaler.

## What We Actually Found

The implementation uncovered three bugs and one fundamental limitation.  The
bugs were fixed.  The limitation is documented here for future work.

---

## Bug 1: CycleSlowly Called Every Sleeping Iteration

### The problem

The `CYCLE` macro in `EmCPU68K.cpp` contained this condition:

```cpp
#define CYCLE(sleeping, cycles)                          \
{                                                        \
    EmHAL::Cycle (sleeping, cycles);                     \
    if (sleeping || ((++counter & 0x7FFF) == 0))         \
    {                                                    \
        this->CycleSlowly (sleeping);                    \
    }                                                    \
}
```

When `sleeping` is true, the `||` short-circuits: `CycleSlowly()` is called on
**every single iteration** of the stopped loop.  `CycleSlowly()` calls
`gettimeofday()` and `usleep()` — system calls that cost microseconds each.
At 16 cycles per sleeping iteration, each `CycleSlowly()` call consumed more
wall time than the emulated time it represented.

### The fix

The stopped loop (`ExecuteStoppedLoop`) no longer uses the `CYCLE` macro.
Instead:

```cpp
const int kSleepCyclesPerIter = 16;

do {
    fCycleCount += kSleepCyclesPerIter;
    EmHAL::Cycle (true, kSleepCyclesPerIter);

    if ((++counter & 0xFFF) == 0)       // every 4096 iterations
        this->CycleSlowly (true);

    // ... interrupt check ...
} while (regs.spcflags & SPCFLAG_STOP);
```

`EmHAL::Cycle()` (cheap: integer add + shift + compare) runs every iteration.
`CycleSlowly()` (expensive: syscalls + throttle) runs every 4096 iterations.

---

## Bug 2: sleepCyclesPerTick Returned 1 Instead of 16

### The problem

`GetSleepCyclesPerTick()` returns `1 << fTmr1Shift`, where `fTmr1Shift` is
set by `PrvUpdateTimerShift()` based on the timer clock source register.

Our initial assumption (from the design document) was that PalmOS uses
**system/16** as the timer clock source, giving `fTmr1Shift = 4` and
`sleepCyclesPerTick = 16`.

Diagnostic output revealed the truth:

```
TIMER SHIFT: controlReg=0x0033  clkSrc=0x0002  -> shift=0
```

PalmOS on the m500 writes `0x0033` to `tmr1Control`:
- Bit 0: timer enable
- Bit 1: interrupt enable
- Bits 2-3: clock source = **0x0002 = System clock** (not system/16)
- Bit 4: free-run mode
- Bit 5: restart on compare

Clock source `0x0002` is `hwrVZ328TmrControlClkSrcSys` — the undivided
system clock.  `PrvUpdateTimerShift()` correctly returns `shift = 0` for
this source.  Then `GetSleepCyclesPerTick()` returns `1 << 0 = 1`.

With `sleepCyclesPerTick = 1`, each sleeping loop iteration advanced
`fCycleCount` by only 1 cycle.  A batch of 4096 iterations = 4096 cycles
= 123 microseconds of emulated time — too small for `usleep()` to be
meaningful.  The throttle couldn't regulate speed.

### The fix

The stopped loop uses a fixed `kSleepCyclesPerIter = 16` regardless of the
timer prescaler configuration.  This is an arbitrary granularity for the
sleeping loop, not a claim about the prescaler.  The Bresenham accumulator
inside `Cycle()` handles the actual prescaler math:

- System clock (shift=0): 16 cycles → 16 timer ticks per iteration
- System/16 (shift=4): 16 cycles → 1 timer tick per iteration

Both produce correct timer advancement at different granularities.

### What this means for the design document

Section 5 of `timer-accuracy-design.md` assumed the m500 uses system/16 and
calculated `compare = 33,161,216 / 16 / 100 = 20,726`.  In reality:

- Clock source: **system clock** (33.16 MHz, no prescaler)
- Compare value: **55,268**
- Tick rate: 33,161,216 / 55,268 = **600 Hz** (not 100 Hz as assumed)

PalmOS on the m500 runs its system tick at 600 Hz, not 100 Hz.

---

## Bug 3: Redundant Platform::Delay() in Stopped Loop

The stopped loop contained a `Platform::Delay()` call that added 10
microseconds of wall-clock delay per iteration, independent of the throttle.
With proper throttling in `CycleSlowly()`, this was redundant and harmful
(it added uncontrolled latency).  Removed.

---

## The Remaining Problem: UAE Cycle Cost Undercount

After fixing all three bugs, the timer fires at the **correct rate** and
speed multipliers produce **correct ratios**.  But the baseline "1x" speed
is approximately 4.78x faster than real hardware.

### Empirical measurement (m500)

| Speed setting | Wall time for 5 animation loops | Expected |
|:---:|:---:|:---:|
| Real m500 hardware | 16.59s | — |
| Emulator at 0.25x | 13.88s | 66.36s |
| Emulator at 0.50x | 7.01s | 33.18s |
| Emulator at 1.00x | ~3.47s (extrapolated) | 16.59s |

The ratio between 0.25x and 0.50x is 13.88/7.01 = 1.98 — confirming the
speed multipliers work correctly.  But our "1x" is ~4.78x real time.

The user's observation: **quarter speed feels almost real-time** —
because 100/25 = 4.0, close to the ~4.78x undercount factor.

### Root cause: How UAE counts cycles

UAE (Universal Amiga Emulator) generates instruction handlers via `gencpu.c`.
Each handler returns a cycle count that is accumulated into `fCycleCount`.
The throttle uses `fCycleCount` and `GetSystemClockFrequency()` to pace
emulated time against wall-clock time.

The cycle count is computed at code-generation time as a sum of:

```
insn_n_cycles = 2                    // base decode cost
+ 2 per word of instruction stream   // gen_nextiword(), gen_nextilong()
+ 2 per byte memory read             // genmovemel(), etc.
+ 2 per word memory read
+ 4 per long memory read
+ special cases (DIVS: +68, DIVS: +72, MUL: +32)
```

This accounting comes from `gencpu.c` lines 1171, 174, 189, 489-491.
Each handler ends with `return insn_n_cycles;` (line 3244).

### How this differs from real MC68000 timing

A real MC68000 uses a **4-cycle minimum bus cycle**: every memory access
(instruction fetch, operand read, operand write) takes at least 4 clock
cycles.  The processor cannot start a new bus cycle in fewer than 4 clocks.

| Operation | UAE cycles | Real MC68000 cycles |
|:---|:---:|:---:|
| Base instruction decode | 2 | 4 (one bus cycle to fetch opcode) |
| Fetch extension word | +2 | +4 (one bus cycle) |
| Byte/word memory read | +2 | +4 (one bus cycle) |
| Long memory read | +4 | +8 (two bus cycles) |
| `MOVE.W D0,D1` (simplest) | 2 | 4 |
| `MOVE.W (A0),D0` | 4 | 8 |
| `MOVE.L (A0)+,(A1)+` | 8 | 20 |
| `ADD.W #imm,D0` | 4 | 8 |

UAE's cycle model is internally consistent but systematically undercounts by
a factor of roughly **2x for simple instructions** and **2.5-5x for complex
memory-intensive instructions**.

### Why this matters

The throttle in `CycleSlowly()` does this:

```cpp
int64_t emulatedUs = (int64_t) elapsed * 1000000LL / clockFreq;
int64_t targetWallUs = emulatedUs * 100 / speed;
int64_t sleepUs = targetWallUs - actualWallUs;
```

`elapsed` is the change in `fCycleCount`.  `clockFreq` is the real hardware
clock (33.16 MHz for m500).  If the CPU executes an instruction that should
cost 8 real cycles but UAE reports only 2, then `fCycleCount` advances by 2
while 8 real cycles' worth of work was done.  The throttle thinks only
2/33,161,216 seconds have passed and doesn't sleep enough.

The timer has the same problem: it advances by `cycles >> shift` ticks, but
`cycles` is the UAE undercount.  The timer fires at the correct wall-clock
rate (because the throttle and timer both use the same undercounted cycles),
but the CPU gets ~4.78x more work done per timer tick than real hardware.

Work-bound operations (animations, UI drawing, computation) appear faster.
Time-bound operations (delays, timeouts) appear correct.

### Why the undercount isn't exactly 2x

The ~4.78x factor is higher than a simple 2x because:

1. **Real MC68000 instructions cost more than 2x UAE's estimate.**  A typical
   PalmOS workload is memory-heavy (ROM/Flash accesses, stack operations,
   structure traversals).  Real memory access costs compound: each bus cycle
   is 4 clocks, and many instructions perform 3-5 bus cycles.

2. **Wait states.**  Real DragonBall processors add wait states for Flash/ROM
   access that the emulator doesn't model.  A Palm m500 adds 1-2 wait states
   per ROM access, effectively doubling the cost of instruction fetches from
   Flash.

3. **Pipeline stalls and prefetch.**  The MC68000 has a 2-word prefetch
   queue.  Branch instructions flush the prefetch, costing extra cycles
   that UAE doesn't account for.

The combined effect: UAE reports ~7 cycles for work that takes ~33 cycles on
real hardware (33/7 ≈ 4.7x).

---

## Diagnostic Data Across Chip Variants

We instrumented all three DragonBall variants and tested with real PalmOS ROMs.

### Timer configuration (all identical)

| Chip | Device | Timer | compare | control | Clock source | shift |
|:---|:---|:---|:---:|:---:|:---|:---:|
| MC68VZ328 | m500 | Timer 1 | 55,268 | 0x0033 | System (33.16 MHz) | 0 |
| MC68EZ328 | Palm V | Timer 1 | 55,268 | 0x0033 | System (16.58 MHz) | 0 |
| MC68328 | Palm III | Timer 2 | 55,268 | 0x0033 | System (16.58 MHz) | 0 |

All three PalmOS ROMs use the undivided system clock (not system/16) and
the same compare value of 55,268.  The resulting tick rates differ because
the system clocks differ:

| Chip | System clock | Tick rate | Tick period |
|:---|:---:|:---:|:---:|
| VZ (m500) | 33,161,216 Hz | 600 Hz | 1,666 us |
| EZ (Palm V) | 16,580,608 Hz | 300 Hz | 3,333 us |
| 328 (Palm III) | 16,580,608 Hz | 300 Hz | 3,333 us |

### Measured timer interrupt rates

**VZ (m500) at 1x speed:**
- Boot phase: ~1,778 us average (expected 1,666 us — 7% slow, throttle working)
- Active animation: bimodal — ~300-500 us (awake bursts) and ~2,000-4,500 us
  (sleeping/throttled)

**EZ (Palm V) at 1x speed:**
- Boot phase: ~3,500-3,680 us average (expected 3,333 us — 5% slow, excellent)
- Active phase: bimodal — ~290-350 us (awake) and ~5,000-7,400 us (sleeping)

**328 (Palm III) at 1x speed:**
- First fire during sleep: 8,203 us (sleeping path, less reliable)
- Active phase: bimodal — ~296-352 us (awake) and ~7,500-12,300 us (sleeping)

### The bimodal pattern explained

The awake path runs the CPU at full host speed between `CycleSlowly()` calls
(every 32,768 instructions via `counter & 0x7FFF`).  During these bursts, the
timer fires at the raw emulation rate (~300 us).  When `CycleSlowly()` runs,
it sleeps to bring the average back to the target rate.  The sleeping path
calls `CycleSlowly()` every 4,096 iterations and produces the longer
intervals.

The overall average is correct — the throttle works.  But individual timer
interrupt intervals swing widely between "too fast" and "compensating sleep."

---

## The Clock Frequency Calculation

Each chip variant computes its system clock from PLL register values
programmed by PalmOS during boot.

**328 and EZ:**
```cpp
uint32 result = 32768L * (14 * (PC + 1) + QC + 1);
```

**VZ:**
```cpp
uint32 result = 32768L * 2 * (14 * (PC + 1) + QC + 1);
```

The VZ formula has a `* 2` factor, which is why it runs at ~33 MHz while the
328 and EZ run at ~16 MHz.  Both formulas derive the frequency from the
32.768 KHz crystal oscillator via the PLL frequency select register.

The VZ additionally supports two optional prescaler dividers and a system
clock scaler (divide by 2, 4, 8, or 16), which PalmOS can configure
independently of the timer prescaler.

---

## What Was Fixed

| Bug | Impact | Fix |
|:---|:---|:---|
| CycleSlowly every sleeping iteration | Sleeping loop 1000x too slow | Batch CycleSlowly every 4096 iters |
| sleepCyclesPerTick = 1 | Sleeping time advancement ~16x too slow | Fixed kSleepCyclesPerIter = 16 |
| Redundant Platform::Delay() | Added uncontrolled 10us per iter | Removed |

## What Remains

The ~4.78x baseline speed offset from UAE's cycle cost model.  The timer
fires at the correct rate.  The speed multipliers work correctly.  The CPU
simply does too much work per tick.

### Possible fix: cycle cost multiplier

Apply a scaling factor to UAE's returned cycle counts before they enter
`fCycleCount`:

```cpp
cycles = (functable[opcode]) (opcode);
cycles *= cycleMultiplier;   // e.g., 5 for m500
fCycleCount += cycles;
CYCLE (false, cycles);
```

This would need to be calibrated per device (the undercount factor depends
on system clock speed, ROM access wait states, and instruction mix).  A
user-configurable setting in the speed menu would allow fine-tuning.

### Alternative: accurate MC68000 cycle tables

Replace UAE's `gencpu.c` cycle model with the real MC68000 cycle counts
from Motorola's MC68000 User Manual, Appendix D.  This is the approach used
by accurate emulators like Hatari and Musashi.  It would fix the problem at
its source but requires significant changes to the UAE code generator.
