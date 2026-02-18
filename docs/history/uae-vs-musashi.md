# UAE vs Musashi: CPU Core Evaluation

## Context

After implementing cycle-accurate timer advancement, we discovered that the
timer fires at the correct rate and speed multipliers work correctly, but the
baseline "1x" speed is ~4.78x faster than real hardware.  The root cause is
UAE's instruction cycle cost model, which systematically undercounts real
MC68000 timing by 2-5x.

This document evaluates whether to replace UAE with Musashi, a cycle-accurate
MC68000 emulator, or apply a simpler correction factor.

## What UAE Does Today

UAE (Universal Amiga Emulator) is POSE's CPU core.  It consists of:

- `gencpu.c` (115K) — a build-time code generator
- `cpuemu.c` (690K) — ~2000 generated opcode handler functions
- `cputbl.c` / `cpustbl.c` (138K) — dispatch tables
- `cpudefs.c` (25K) — instruction definition data
- Various headers and glue code

Each generated handler returns a cycle count.  The cycle model in `gencpu.c`:

```c
insn_n_cycles = 2;                        // base decode cost
// + 2 per instruction word fetched        (gen_nextiword)
// + 2 per byte memory read
// + 2 per word memory read
// + 4 per long memory read
// + special cases (DIVS: +68, MULS: +32)
```

These counts are compiled into the handlers as constants (`return 4;`,
`return 8;`, etc.) and returned to the caller at runtime.

## What Musashi Offers

Musashi (https://github.com/kstenerud/Musashi) is a portable MC68000/010/020/
030/040 emulator written in ANSI C89.  MIT license.  It uses hand-written
instruction implementations with cycle counts taken directly from Motorola's
MC68000 User Manual.

Key architectural differences:

- **Cycle-budget execution:** `m68k_execute(num_cycles)` runs instructions
  until the cycle budget is exhausted, then returns actual cycles consumed.
  This is the standard model for accurate emulators.

- **Lookup table cycle costs:** `CYC_INSTRUCTION[opcode]` is a 65536-entry
  table with per-CPU-type cycle counts, selected at init time.  The values
  come from the Motorola reference manual.

- **Callback-based memory:** Host implements `m68k_read_memory_8/16/32()`
  and `m68k_write_memory_8/16/32()`.  Conceptually identical to UAE's
  `EmAddressBank` dispatch but with a different function signature.

- **Callback hooks:** `M68K_INSTRUCTION_CALLBACK`, `M68K_RTE_CALLBACK`,
  `M68K_RESET_CALLBACK`, `M68K_BKPT_ACK_CALLBACK`, etc.  Similar to
  POSE's `ProcessJSR`, `ProcessRTS`, `ProcessException` hooks.

## Cycle Accuracy Comparison

| Instruction | UAE | Musashi (68000) | Real MC68000 |
|:---|:---:|:---:|:---:|
| `MOVE.W D0,D1` | 2 | 4 | 4 |
| `MOVE.W (A0),D0` | 4 | 8 | 8 |
| `MOVE.L (A0)+,(A1)+` | 8 | 20 | 20 |
| `ADD.W #imm,D0` | 4 | 8 | 8 |
| `NOP` | 2 | 4 | 4 |
| `JSR abs` | ~6 | 12+ (by EA) | 12+ (by EA) |
| `RTS` | ~4 | 16 | 16 |
| `BRA` | ~4 | 10 | 10 |
| `DBRA` (taken) | ~4 | 12 | 12 |
| `LEA (d,An)` | ~4 | 8 | 8 |

UAE undercounts by 2x for simple register operations and 2.5-5x for
memory-intensive instructions.  Musashi matches the Motorola reference
exactly.

### Why the undercount isn't a flat ratio

UAE's base decode cost is 2 (should be 4: one bus cycle).  Memory access
cost is +2 per word (should be +4: one bus cycle per word).  The error
compounds with instruction complexity:

- Register-only instructions: 2x undercount (2 vs 4)
- One memory operand: ~2.5x undercount (4 vs 8-12)
- Two memory operands: ~3-4x undercount (8 vs 20-28)
- Complex instructions (MOVEM, MUL, DIV): closer to correct (special-cased)

A typical PalmOS workload is memory-heavy (ROM fetches, stack operations,
structure traversals), pushing the average undercount toward ~4-5x.  This
matches our measured ~4.78x baseline offset on the m500.

## Benefits of Switching to Musashi

1. **Correct cycle counts from Motorola's reference manual.**  No multiplier
   hack.  Timer, throttle, and speed menu all work at the correct baseline.

2. **Per-CPU-type cycle tables.**  Separate columns for 68000/010/020/040.
   If we ever emulate different processor variants, timing adjusts
   automatically.

3. **Cycle-budget execution model.**  `m68k_execute(num_cycles)` maps
   directly to "run N cycles, then check timers" — the standard approach
   in accurate emulators.  Cleaner than UAE's "run one instruction, return
   cost, caller accumulates" model.

4. **MIT license** vs UAE's GPL.

5. **Simpler build.**  No code generator step (`gencpu.c` → `cpuemu.c`).
   Musashi uses `m68kmake` to generate dispatch tables, but instruction
   implementations are hand-written in `m68k_in.c`.

6. **Actively maintained reference.**  Musashi is used in MAME and other
   well-tested emulators.

## Cost of Switching

### Integration work

1. **Memory callbacks.**  Wire `m68k_read_memory_8/16/32()` and
   `m68k_write_memory_8/16/32()` to POSE's `EmAddressBank` dispatch.
   Conceptually straightforward — same bank-based routing, different
   function signatures.

2. **Exception/trap hooks.**  POSE hooks deeply into UAE:
   - `ProcessJSR()` / `ProcessJSR_Ind()` — track subroutine calls
   - `ProcessRTS()` / `ProcessRTE()` — track returns
   - `ProcessException()` — handle exceptions
   - `CheckNewPC()` / `CheckNewSP()` — validate PC/SP changes
   - Stack overflow monitoring (`gStackHigh`, `gStackLow`)

   Musashi provides `M68K_INSTRUCTION_CALLBACK`, `M68K_RTE_CALLBACK`,
   and a few others, but they're not 1:1.  JSR/RTS tracking would need
   the instruction callback to inspect the current opcode.

3. **Register access.**  UAE: `regs.d[n]`, `m68k_areg()`, `regs.pc`,
   `regs.spcflags`.  Musashi: `REG_D[n]`, `REG_A[n]`, `REG_PC`,
   `CPU_STOPPED`.  Hundreds of references throughout EmCPU68K.cpp and
   other files need updating.

4. **Special flags and STOP handling.**  UAE uses `SPCFLAG_STOP`,
   `SPCFLAG_INT`, `SPCFLAG_BRK` with explicit flag checks in the
   execute loop.  Musashi handles STOP internally and uses
   `m68k_set_irq()` for interrupts.  The entire `ExecuteStoppedLoop()`
   and interrupt dispatch logic would change.

5. **Profiling integration.**  POSE's `HAS_PROFILING` / `perftbl[]`
   system is woven into UAE's generated code.  Would need reimplementation
   via Musashi's instruction callback, likely with a performance cost.

6. **No incremental migration path.**  UAE's 690K of generated handlers
   are replaced wholesale by Musashi's ~150K of hand-written code.  The
   entire CPU core changes at once.  There is no way to run "half UAE,
   half Musashi."

### Risk

- Subtle regressions in trap/exception handling (PalmOS relies heavily
  on traps for system calls)
- Performance characteristics may differ (Musashi's dispatch is different
  from UAE's; may be faster or slower depending on host CPU cache behavior)
- Every existing test case exercises UAE's behavior; all would need
  re-validation

### Estimated effort

A week or more of careful integration work, plus extensive testing across
ROM images and device types.

## The Alternative: Cycle Correction Factor

Instead of replacing UAE, apply a scaling multiplier to UAE's cycle returns:

```cpp
cycles = (functable[opcode]) (opcode);
cycles *= kCycleCorrectionFactor;   // e.g., 5
fCycleCount += cycles;
CYCLE (false, cycles);
```

### Pros

- One line of code.  Works immediately.
- No risk to trap/exception/profiling integration.
- Could be made per-device (different multipliers for different clock speeds).
- Could be user-configurable for fine-tuning.

### Cons

- A single multiplier cannot correct the nonlinear error.  Register-heavy
  code is undercounted by ~2x; memory-heavy code by ~4-5x.  A multiplier
  of 5 would make register-heavy workloads ~2.5x too slow while
  memory-heavy workloads would be roughly correct.
- Variation of roughly ±30% depending on instruction mix.
- It's a hack that papers over the real problem.

### A refinement: two-tier multiplier

Since UAE already returns different costs for different instruction types
(the cost increases with memory operands), a single multiplier actually
preserves relative differences.  The error is more uniform than it first
appears:

- UAE's `NOP` (2) × 2 = 4 (correct)
- UAE's `MOVE.W (A0),D0` (4) × 2 = 8 (correct)
- UAE's `MOVE.L (A0)+,(A1)+` (8) × 2.5 = 20 (correct)

A multiplier of ~2.0-2.5 gets surprisingly close across most instructions.
The base decode cost of 2 (should be 4) is the dominant error, and memory
access costs of +2 (should be +4) scale linearly.  A multiplier of 2
corrects the base; the remaining error is in the memory access delta.

However, wait states for ROM/Flash access on real hardware (1-2 extra
cycles per bus cycle) push the effective multiplier higher for ROM-heavy
workloads — which is almost everything PalmOS does.

## Recommendation

**Short term:** Apply a cycle correction factor of ~2.0 with a
user-configurable adjustment.  This gets baseline speed within ~20-30% of
real hardware with minimal risk.  Expose it in the speed menu or as an
advanced setting.

**Long term:** Musashi is the correct solution.  It would make timing
trustworthy across all workloads and instruction mixes.  But it's a large,
risky change that should be done as a dedicated project with thorough
testing, not as a side-effect of the timer accuracy work.

The two approaches are not mutually exclusive.  The correction factor
works today and can be removed when/if Musashi is integrated.
