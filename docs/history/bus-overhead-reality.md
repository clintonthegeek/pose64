# Bus Overhead: Why A Single Coefficient Can't Work

## What We Tried

Added a per-device "bus overhead" multiplier to POSE's cycle counting:

```
cycles = UAE_half_cycles * 2 * coefficient
```

Applied uniformly to every instruction. Fed to both the throttle (wall-clock
pacing) and the timer (hardware counter advancement).

## What Happened

- Tuned coefficient to 8.8x on m500 to sync the Welcome animation with real hardware
- Redraws became visibly too slow compared to real device
- Coefficient of ~6.0x (measured time ratio) was too fast for animations
- No single value works for both CPU-bound and mixed workloads

## Why It Fails

### The Timer Problem

The coefficient inflates cycle counts fed to both the throttle and the timer.
The throttle sleeps proportionally longer (CPU-bound work slows down). But the
timer accumulates inflated cycles AND the throttle stretches wall time by the
same factor — they cancel out. Timer interrupts fire at the same wall-clock
rate regardless of coefficient.

On real hardware, this is actually correct: the timer crystal doesn't care about
bus wait states. But it means the coefficient is invisible to timer-driven code.
To make a mixed CPU/timer workload match, you have to over-inflate the
coefficient, which breaks pure CPU workloads.

### What Real Hardware Actually Does

The Dragonball VZ doesn't have one "bus overhead." It has many:

| Memory Region | Access Type | Wait States | Typical Latency |
|---------------|-------------|-------------|-----------------|
| Flash ROM | Read (code fetch) | Chip-select configured | Slow |
| Flash ROM | Read (data) | Same CS, different pattern | Slow |
| SRAM | Read/Write | Different CS config | Fast |
| Hardware registers | Read/Write | I/O bus timing | Variable |
| LCD controller | Write | Framebuffer mapped | Variable |

Every single bus access in every single instruction has a different penalty
depending on WHERE in the address space it touches. A `MOVE.L (A0),D0` reading
from ROM is slower than the same instruction reading from RAM. Code fetches
(every instruction) hit ROM. Stack operations hit RAM. Screen writes hit the
LCD framebuffer.

### What UAE Gets Wrong

UAE's opcode handlers return cycle counts based on the MC68000 programmer's
manual, but:

- Not all instructions have perfectly accurate counts
- The `* 2` half-cycle conversion is an approximation
- Memory access counts per instruction aren't tracked separately
- There's no distinction between code fetch, data read, and data write cycles

### What the MC68000 Manual Assumes

The manual's cycle counts assume minimum bus timing: 4 clock cycles per bus
access (2 clocks for address, 2 for data). Real Dragonball hardware inserts
wait states that stretch each bus access. The manual counts are a lower bound.

## What Correct Emulation Would Need

### Per-Region Wait State Emulation

Read the chip-select registers (csAGroupBase, csASelect, etc.) to determine
the configured wait states for each address range. Apply the correct penalty
to each memory access based on the target address.

This means the cycle cost of an instruction depends on its operand addresses,
not just the opcode. `MOVE.L (A0),D0` costs different amounts depending on
what A0 points to.

### Separate Bus Access Counting

For each instruction, track:
- Number of code fetches (opcode + extension words)
- Number of data reads (operands)
- Number of data writes (results)
- Address region for each access

Apply per-region wait states to each access independently.

### On-Device Benchmarking

To validate any of this, you'd need software running on real hardware that:

1. **Cycle-counts individual operations** using the Dragonball's built-in
   timer as a high-resolution clock. Read the timer counter before and after
   a known instruction sequence.

2. **Tests each memory region** — run the same instruction sequence with
   operands in ROM, RAM, and I/O space. Measure the difference.

3. **Measures aggregate throughput** — time large loops of known instruction
   mixes to get an average CPI (clocks per instruction) figure.

4. **Captures timer vs CPU ratio** — run a computation for a known wall-clock
   duration (timed by hardware timer) and count how many instructions executed.

This gives you the real numbers: ROM wait states, RAM wait states, effective
CPI for different instruction mixes. Different devices (328 vs EZ vs VZ) will
have completely different numbers.

### Per-Device Data Needed

For each device family:

| Parameter | How to Measure |
|-----------|---------------|
| ROM read wait states | Time a tight loop in ROM vs RAM |
| RAM read wait states | Time reads from different RAM regions |
| RAM write wait states | Time writes vs reads |
| I/O access penalty | Time hardware register access loops |
| Code fetch penalty | Time NOP slides in ROM vs RAM |
| Effective CPI (ROM code, RAM data) | Time a realistic instruction mix |
| Effective CPI (RAM code, RAM data) | Same mix from RAM |
| Timer ticks per N instructions | Calibrate timer against instruction count |

## Current State

The bus overhead coefficient exists as a per-device setting in the UI
(Emulation Speed → Bus Overhead). It's a single multiplier that uniformly
scales all cycle counts. It's useful for rough approximation but fundamentally
cannot match real hardware for all workloads simultaneously.

The code is in place. The infrastructure (per-device preference, dialog, CPU
loop integration) works. If someone does the on-device benchmarking, the single
coefficient could be replaced with per-region wait state injection without
changing the rest of the plumbing.

## Files

- `EmCPU68K.cpp:482` — cycle accumulation with overhead
- `EmSession.cpp:301` — per-device overhead loaded at session init
- `EmDlgQt.cpp:1716` — per-device coefficient grid dialog
- `EmApplication.cpp:1137` — dialog handler, preference serialization
- `PreferenceMgr.h:416` — BusOverhead string preference
