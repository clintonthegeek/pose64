# BenchPRC Results: Palm m500 vs POSE

What the numbers say about the machine in your hand, and why the emulator
doesn't match it.

## The Big Surprise

The whole reason we built BenchPRC was the assumption that POSE runs too fast
-- that the emulator completes operations quicker than real hardware because it
doesn't model bus wait states. The Welcome animation ran 3.56x faster in POSE
than on a real m500.

The benchmark says the opposite. **POSE is slower than the real Palm m500 for
every single operation.**

| Test | Real m500 | POSE | POSE / Real |
|------|:---------:|:----:|:-----------:|
| RAM Rd W | 735 | 1910 | **2.60x** |
| RAM Rd L | 888 | 2590 | **2.92x** |
| RAM Wr W | 838 | 2498 | **2.98x** |
| RAM Wr L | 831 | 2569 | **3.09x** |
| ROM Rd W | 535 | 1910 | **3.57x** |
| ROM Rd L | 684 | 2497 | **3.65x** |
| HW Reg Rd | 504 | 1982 | **3.93x** |
| NOP ROM | 252 | 807 | **3.20x** |
| NOP RAM | 42 | 156 | **3.71x** |
| LCD Wr | 210 | 624 | **2.97x** |
| Stack Ops | 2693 | 7872 | **2.92x** |
| Mixed CPI | 1214 | 3232 | **2.66x** |
| Calib 1M | 36 | 121 | **3.36x** |

*(All tests at 5x iterations = 1,000,000 x 8 ops. POSE running at 1x Realtime
with Accurate Timer, Bus Overhead 1.0. Ticks at 100 Hz on both platforms.)*

POSE takes 2.6x to 3.9x more emulated ticks than real hardware for the same
operations. The emulator overcounts cycles.

### Why the Welcome animation looked faster

The Welcome animation speed test (3.56x too fast) was measured with a stopwatch
against wall-clock time, using the **old timer mechanism** (fixed `increment=4`
per CycleSlowly check). That timer advanced at a rate decoupled from actual
instruction cycles -- it just incremented a counter every 32,768 instructions
regardless of how many cycles those instructions cost. Timer-driven delays
(like the pauses between animation frames) were too short because the timer
fired too often relative to wall time.

With the **accurate timer** (which advances based on actual CPU cycle count),
the benchmark reveals the truth: POSE's per-instruction timing is slower than
real hardware, not faster. The accurate timer fixed the timer problem but
exposed the cycle-counting problem.

## Inside Your Palm m500

The m500's brain is a Motorola MC68VZ328 "DragonBall VZ" -- a system-on-chip
built around a 68EC000 CPU core running at 33.16 MHz. Every instruction the CPU
executes touches the bus at least once (to fetch the instruction itself), and
most instructions touch it multiple times. What makes the m500 interesting --
and hard to emulate -- is that different regions of the address space have
completely different bus timing.

### The address space

The benchmark reports the actual addresses it used:

```
RAM:  0x0000554C    heap memory - main working memory
ROM:  0x1001FE44    Flash - the PalmOS itself and all apps live here
LCD:  0x000348CA    framebuffer - memory-mapped display
HWR:  0xFFFFF614    hardware registers - timer, interrupt controller, GPIO
```

These four addresses hit four different pieces of silicon inside the VZ328,
each with its own bus interface and timing characteristics:

**Flash ROM (0x10xxxxxx)** -- A NOR Flash chip wired to the VZ328's chip-select
lines. Every instruction the CPU executes is fetched from here (since PalmOS
and all apps live in Flash). This is the single most important bus region for
overall speed because *every single instruction* requires at least one Flash
read.

**SDRAM (0x00000000-0x001FFFFF)** -- The m500 uses synchronous DRAM, not the
static RAM of earlier Palms. SDRAM is cheap and dense but has CAS latency (the
delay between sending a column address and getting data back) plus periodic
refresh cycles that steal bus bandwidth. Individual random reads are slow;
sequential burst reads are fast.

**LCD framebuffer (0x0003xxxx)** -- Memory-mapped within the SDRAM address
range but writes go through the LCD controller's bus interface. On the m500
this maps to a region of the same SDRAM, so LCD writes have similar timing to
RAM writes.

**Hardware registers (0xFFFFF000-0xFFFFFFFF)** -- The VZ328's own control
registers: timers, interrupt controller, UART, GPIO, PLL, chip-select
configuration. These are internal to the chip and bypass the external bus
entirely.

### What each test actually does (MC68000 instruction level)

The benchmark's C code compiles to specific m68k instructions. Here's what's
actually happening on the bus:

**RAM/ROM Read Word** (`sink = p[0]; sink = p[1]; ...`):
Each compiles to `MOVE.W d16(An),Dn` -- a 4-byte instruction (2 words). Per
operation: 2 code fetches from Flash + 1 data read from the target region.
Three bus cycles total. UAE returns 6 half-cycles, x2 = 12 emulated cycles.
MC68000 manual: 12(3/0).

**RAM/ROM Read Long** (`sink = p[0]; ...` with 32-bit pointer):
`MOVE.L d16(An),Dn` -- same 4-byte instruction. Per operation: 2 code fetches
from Flash + 2 data reads (32-bit = two 16-bit bus cycles). Four bus cycles.
UAE returns 8, x2 = 16 emulated cycles. MC68000 manual: 16(4/0).

**NOP ROM** (`asm volatile("nop")`):
`NOP` opcode 0x4E71 -- 2-byte instruction. One code fetch from Flash. UAE
returns 2, x2 = 4 emulated cycles. MC68000 manual: 4(1/0).

**NOP RAM** (hand-assembled NOP slide in heap):
Same `NOP` opcode, but fetched from SDRAM. One code fetch from RAM per NOP.
Same 4 emulated cycles, but on real hardware the fetch time depends on SDRAM
timing instead of Flash timing.

**HW Reg Read** (`sink = *reg` from 0xFFFFF614):
`MOVE.W (An),Dn` reading the Timer 2 counter register. 2 code fetches from
Flash + 1 read from the hardware register space.

## The Region Problem

### POSE is blind

In the emulator, every memory access costs the same number of cycles regardless
of where it goes. Look at the POSE column:

| Test | POSE ticks |
|------|:----------:|
| RAM Rd W | 1910 |
| ROM Rd W | **1910** |
| HW Reg Rd | 1982 |

RAM reads and ROM reads are **identical** (1910 = 1910). The HW register
read is slightly different (1982) only because it uses a different addressing
mode (`(An)` vs `d16(An)`), not because of bus timing. UAE's opcode handlers
return the MC68000 programmer's manual cycle count for each instruction, and
that count depends only on the opcode and addressing mode, never on the target
address.

### Your Palm sees the world differently

On real hardware, the same three tests:

| Test | Real ticks | vs RAM Rd W |
|------|:----------:|:-----------:|
| RAM Rd W | 735 | 1.00x |
| ROM Rd W | 535 | **0.73x** |
| HW Reg Rd | 504 | **0.69x** |

ROM data reads are **27% faster** than RAM reads. Hardware register reads are
**31% faster** than RAM reads. This is the opposite of the naive expectation
that "Flash is slow, RAM is fast."

### Why ROM reads beat RAM reads

This is the most surprising result and it reveals something fundamental about
the m500's bus architecture.

Both the RAM read test and the ROM read test have the same code -- the same C
`for` loop compiled into the same instruction sequence, fetched from the same
Flash address. The *only* difference is where the data operand points.

When code is in Flash and data is also in Flash (ROM Rd test), the bus stays on
the same chip-select group for both fetches and data reads. The Flash chip can
respond quickly to back-to-back accesses because it's already selected.

When code is in Flash but data is in SDRAM (RAM Rd test), the bus controller
must switch chip-select lines between every code fetch and every data access.
Each switch incurs bus turnaround time. Plus, the SDRAM has CAS latency on
every access -- there's a delay between the address arriving and the data
appearing on the bus.

The SDRAM in the m500 is a general-purpose commodity chip optimised for bulk
throughput (reads and writes in bursts), not for random single-word accesses.
Each random access pays the CAS latency penalty. Flash, by contrast, has a
simpler interface and the VZ328's chip-select timings may be configured for
minimal wait states.

### Code fetch is everything

The single biggest timing factor is where instructions are fetched from:

| Test | Real ticks |
|------|:----------:|
| NOP ROM (code from Flash) | 252 |
| NOP RAM (code from SDRAM) | 42 |

Running identical NOP instructions from RAM is **6x faster** than from Flash.
Every instruction the m500 executes normally comes from Flash. This is the
dominant cost in real-world execution. The MC68VZ328 doesn't have an instruction
cache -- every single instruction word is fetched from Flash every time.

But there's a subtlety: the benchmark's NOP RAM test uses a different loop
structure (hand-assembled `DBRA` in a heap buffer) than NOP ROM (compiler-
generated `for` loop from Flash), so the 6x ratio includes both the code-fetch
speedup and the loop structure difference. The actual code-fetch penalty is
likely 3-5x, not a full 6x.

### The full region map

Here's what the m500's bus looks like from the benchmark data, normalised to
the fastest operation:

| Operation | Real ticks | Relative speed |
|-----------|:----------:|:--------------:|
| HW Reg Rd | 504 | Fastest (1.00x) |
| ROM Rd W | 535 | 1.06x |
| NOP ROM (code fetch) | 252* | — |
| NOP RAM (code fetch) | 42* | — |
| RAM Rd W | 735 | 1.46x |
| RAM Wr W | 838 | 1.66x |
| RAM Rd L | 888 | 1.76x |
| LCD Wr (scaled) | 840 | 1.67x |
| Stack Ops (scaled) | 10772 | 21.4x |

*(\*NOP tests measure code fetch + loop overhead combined, not directly
comparable to data-access tests)*

Stack operations are spectacularly slow (21x the HW register baseline) because
each "operation" is a volatile 32-bit read + ALU + 32-bit write, all through
stack-relative (`d16(SP)`) addressing, with every access hitting SDRAM while
code is fetched from Flash. The addressing mode itself costs more cycles, and
every bus access pays the SDRAM penalty.

## What This Means for Emulation

### The x2 multiplier may be wrong

UAE's opcode handlers return values that we multiply by 2, on the assumption
they're "half-cycles." For every instruction checked, the resulting value
matches the MC68000 programmer's manual exactly:

| Instruction | UAE returns | x2 | Manual |
|-------------|:-----------:|:--:|:------:|
| NOP | 2 | 4 | 4(1/0) |
| MOVE.W (An),Dn | 4 | 8 | 8(2/0) |
| MOVE.W (d16,An),Dn | 6 | 12 | 12(3/0) |
| MOVE.L (d16,An),Dn | 8 | 16 | 16(4/0) |

But the emulator is 2.6-3.9x slower than real hardware. There are three
possible explanations:

1. **The VZ328 is faster than the MC68000 spec.** The DragonBall VZ uses a
   68EC000 core, a later revision with some pipeline improvements. It may
   execute certain instructions in fewer cycles than the original 1979 MC68000
   manual specifies. At 33 MHz (vs the original 8 MHz design), the core may
   overlap bus phases more efficiently.

2. **UAE's half-cycle values are already actual cycles, and the x2 is
   double-counting.** If UAE returns actual cycle counts (not half-cycles),
   then every instruction is billed at 2x its true cost. This alone would
   explain much of the 2.6-3.9x slowdown.

3. **Both.** UAE returns conservative cycle counts that are close to but not
   exactly half the manual values, and the x2 overshoots.

Without disassembling the 68EC000's microcode or running many more
single-instruction benchmarks, we can't distinguish between these. But the
practical takeaway is clear: **the emulator's cycle counting needs to come down
by a factor of ~3**, and it needs to come down *differently* for different
memory regions.

### What a correct fix would look like

The benchmark gives us the raw data for per-region correction. For the m500:

| Region | Real/POSE ratio | Meaning |
|--------|:---------------:|---------|
| RAM data ops | 0.35-0.39 | POSE 2.6-2.9x too slow |
| ROM data ops | 0.27-0.28 | POSE 3.6-3.7x too slow |
| HW register ops | 0.25 | POSE 3.9x too slow |
| Code fetch (Flash) | 0.31 | POSE 3.2x too slow |
| Code fetch (RAM) | 0.27 | POSE 3.7x too slow |
| LCD writes | 0.34 | POSE 3.0x too slow |
| Mixed realistic | 0.38 | POSE 2.7x too slow |

These aren't correction factors to *add* wait states -- they're factors to
*remove* overcounting. The emulator needs to run *faster*, not slower.

A simple approach: divide the x2 multiplier by the test-specific ratio. But
even that wouldn't work because each instruction involves bus accesses to
*multiple* regions simultaneously (code fetch from Flash + data from wherever).
The correct fix requires decomposing each instruction into its individual bus
accesses and applying a per-region scaling factor to each one.

### The single-coefficient problem, revisited

We already knew from `docs/bus-overhead-reality.md` that a single coefficient
can't model all workloads. The benchmark data makes this concrete. To match
real hardware for:

- **Pure RAM data work**: coefficient = 0.38 (speed up 2.6x)
- **Pure ROM data work**: coefficient = 0.28 (speed up 3.6x)
- **Code execution from Flash**: coefficient = 0.31 (speed up 3.2x)
- **Mixed realistic code**: coefficient = 0.38 (speed up 2.7x)

No single number covers this range. And these are all speed-up factors (< 1.0),
meaning the Bus Overhead coefficient should be set *below* 1.0 to make the
emulator faster, not above 1.0 to slow it down.

The earlier finding that 8.8x "synced" the Welcome animation was an artifact
of interacting with the throttle mechanism: inflating cycle counts made the
throttle sleep more, which accidentally shortened timer-driven delays relative
to CPU-bound work (because both are measured in emulated cycles, but only the
CPU-bound part gets stretched in wall time). It was a coincidence, not a
correction.

## Data for Other Devices

BenchPRC is designed to be run on any Palm. The expected results for other
DragonBall variants:

| Chip | Example devices | Expected differences from m500 |
|------|-----------------|-------------------------------|
| MC68328 | Palm III | 16.58 MHz clock, SRAM (not SDRAM), different Flash. RAM should be much faster relative to ROM. |
| MC68EZ328 | Palm V, IIIx | 16.58 MHz, may use SRAM. Similar to 328 but with some peripheral improvements. |
| MC68VZ328 | m505, i705 | Same chip as m500, expect similar numbers. Different ROM may affect code-fetch timing. |

The m500 data is for one specific device. Each device family needs its own
benchmark run to calibrate the emulator's per-region timing. Install
`BenchPRC.prc`, run at 5x, photograph the screen.

## Raw Data

### Real Palm m500

```
N=1000000x8, Tick Rate: 100/s, Run at 5x

RAM Rd W      735
RAM Rd L      888
RAM Wr W      838
RAM Wr L      831
ROM Rd W      535
ROM Rd L      684
HW Reg Rd     504
NOP ROM       252
NOP RAM        42
LCD Wr        210  (N=250000x8)
Stack Ops    2693
Mixed CPI    1214
Calib 1M       36

Addresses: RAM:0x0000554C  ROM:0x1001FE44  LCD:0x000348CA  HWR:0xFFFFF614
```

### POSE (Accurate Timer, 1x Realtime, Bus Overhead 1.0)

```
N=1000000x8, Tick Rate: 100/s, Run at 5x

RAM Rd W     1910
RAM Rd L     2590
RAM Wr W     2498
RAM Wr L     2569
ROM Rd W     1910
ROM Rd L     2497
HW Reg Rd    1982
NOP ROM       807
NOP RAM       156
LCD Wr        624  (N=250000x8)
Stack Ops    7872
Mixed CPI    3232
Calib 1M      121

Addresses: RAM:0x00005446  ROM:0x1001FE44  LCD:0x00034A5E  HWR:0xFFFFF614
```

## Files

- `benchmarkPRC/src/bench.c` -- benchmark source code
- `benchmarkPRC/real_palm_m500.txt` -- real device results
- `benchmarkPRC/emulator_m500.txt` -- POSE results
- `src/core/UAE/cpuemu.c` -- UAE opcode handlers (cycle return values)
- `src/core/Hardware/EmCPU68K.cpp:480` -- cycle accumulation (`cycles * 2 * overhead / 100`)
- `src/core/Hardware/EmRegsVZ.cpp:1414` -- `GetSystemClockFrequency()` (33.16 MHz from PLL)
