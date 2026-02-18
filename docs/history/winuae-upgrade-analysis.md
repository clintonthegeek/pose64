# WinUAE CPU Core Upgrade Analysis

## Executive Summary

POSE uses UAE 0.8.10 (~1999).  WinUAE has evolved the same codebase for 25
years into a 100% cycle-accurate 68000 emulator.  The core architecture
(`gencpu` → `cpuemu`, callback memory, `regs` struct, SPCFLAG dispatch) is
the same.  The cycle counting model is completely different and correct.

Upgrading the `gencpu` code generator from WinUAE and regenerating `cpuemu`
would give us accurate 68000 cycle counts with minimal changes to the rest
of POSE's integration layer.  We only need the 68000 "simple cycles" mode —
not the cycle-exact, MMU, 68020+, or JIT features that make up 95% of
WinUAE's bulk.

## Version Gap

| | POSE (UAE 0.8.10) | WinUAE (2024) |
|:---|:---|:---|
| Year | ~1999 | 2024 |
| `gencpu` | 3,346 lines (.c) | 10,329 lines (.cpp) |
| `cpuemu` | 24,243 lines (1 file) | 18 files, ~1M lines total |
| 68000 accuracy | ~50% (2-cycle bus) | 100% verified |
| CPU variants | 68000 only | 68000-68060 |
| Cycle modes | Static returns | Simple, prefetch, cycle-exact |
| Timing source | Ad-hoc | Markt & Technik published tables |

## The Cycle Counting Difference

### POSE UAE 0.8.10

```c
// gencpu.c line 1171:
insn_n_cycles = 2;                    // base decode: 2 (should be 4)

// Memory reads (line 489-491):
case sz_byte: insn_n_cycles += 2;     // should be 4
case sz_word: insn_n_cycles += 2;     // should be 4
case sz_long: insn_n_cycles += 4;     // should be 8

// Return (line 3244):
printf ("return %d;\n", insn_n_cycles);  // static constant
```

Every cycle cost is exactly half the real MC68000 value.

### WinUAE

```cpp
// gencpu.cpp line 5038:
insn_n_cycles = 0;                    // starts at 0

// Memory accesses tracked via separate counters:
count_readw++;    // word reads
count_writew++;   // word writes
count_readl++;    // long reads
count_writel++;   // long writes

// Final return (gencpu.cpp line 764):
// cost = (count_readw + count_writew) * 4
//      + (count_readl + count_writel) * 8
//      + insn_n_cycles
returncycles(total);

// In generated code:
return N * CYCLE_UNIT / 2;           // CYCLE_UNIT = 512
```

WinUAE uses **4 cycles per word bus access** and **8 cycles per long bus
access** — the real MC68000 bus timing.  The `CYCLE_UNIT = 512` scaling
factor provides sub-cycle precision for the cycle-exact modes.

### Concrete examples

| Instruction | POSE return | WinUAE return | Real MC68000 |
|:---|:---:|:---:|:---:|
| `NOP` | 2 | 4×256=1024 | 4 |
| `MOVE.W D0,D1` | 2 | 4×256=1024 | 4 |
| `MOVE.W (A0),D0` | 4 | 8×256=2048 | 8 |
| `MOVE.L (A0)+,(A1)+` | 8 | 20×256=5120 | 20 |
| `ADD.W #imm,D0` | 4 | 8×256=2048 | 8 |
| `RTS` | ~4 | 16×256=4096 | 16 |
| `BRA` | ~4 | 10×256=2560 | 10 |

(WinUAE returns are in CYCLE_UNIT-scaled form; dividing by 256 gives real
cycle counts.)

## What We Need vs What WinUAE Has

WinUAE generates 18 different `cpuemu_*.cpp` files for different CPU models
and accuracy levels.  We only need **one**: the 68000 with simple (non-CE)
cycle counting.

### WinUAE cpuemu file map

| File | CPU | Mode | Lines | Need? |
|:---|:---|:---|:---:|:---:|
| `cpuemu_0.cpp` | 68000 | Base | 64,827 | **Candidate** |
| `cpuemu_11.cpp` | 68010 | Prefetch | 159,930 | No |
| `cpuemu_13.cpp` | 68010 | Cycle-exact | 161,999 | No |
| `cpuemu_20.cpp` | 68020 | Prefetch | 41,818 | No |
| `cpuemu_21.cpp` | 68020 | Cycle-exact | 45,198 | No |
| `cpuemu_22.cpp` | 68030 | Prefetch | 42,034 | No |
| `cpuemu_23.cpp` | 68030 | Cycle-exact | 45,421 | No |
| `cpuemu_24.cpp` | 68040/060 | Cycle-exact | 42,661 | No |
| `cpuemu_31.cpp` | 68040 | MMU | 43,587 | No |
| `cpuemu_32.cpp` | 68030 | MMU | 43,780 | No |
| `cpuemu_33.cpp` | 68060 | MMU | 41,986 | No |
| `cpuemu_34.cpp` | 68030 | MMU+cache | 45,837 | No |
| `cpuemu_35.cpp` | 68030 | MMU+cache+CE | 49,222 | No |
| `cpuemu_40.cpp` | Generic | Direct | 84,846 | **Maybe** |
| `cpuemu_50.cpp` | Generic | Indirect | 61,740 | No |

`cpuemu_0.cpp` is the 68000 base emulation — the most likely candidate.
`cpuemu_40.cpp` (generic direct) might also work.

## Architecture Comparison

### What stayed the same (easy to integrate)

1. **Opcode dispatch pattern:**
   - POSE: `cycles = (*cpufunctbl[opcode])(opcode);`
   - WinUAE: `cpu_cycles = (*cpufunctbl[r->opcode])(r->opcode) & 0xffff;`
   - Same function pointer table, same basic pattern.

2. **Register structure:**
   - Both use `regstruct regs` with `regs.regs[16]`, `regs.pc`,
     `regs.spcflags`, etc.
   - WinUAE adds many fields (prefetch020, ipl[], cacheholdingaddr, etc.)
     but the core 68000 fields are compatible.

3. **SPCFLAG mechanism:**
   - POSE: `SPCFLAG_STOP=2, SPCFLAG_INT=8, SPCFLAG_BRK=16`
   - WinUAE: Same names, same mechanism, different values.

4. **Memory access macros:**
   - POSE: `get_byte()`, `get_word()`, `get_long()`, `put_byte()`, etc.
   - WinUAE: Same names in generated code, routed through function pointers
     (`x_get_word`, `x_put_word`) for mode flexibility.

5. **Handler function signature:**
   - POSE: `unsigned long REGPARAM2 op_XXXX_3(uae_u32 opcode)`
   - WinUAE: `unsigned long REGPARAM2 op_XXXX_0_ff(uae_u32 opcode)`
   - Same return type, same parameter. The `_ff` suffix and table number
     differ.

### What changed (integration work needed)

1. **Return value encoding:**
   - POSE: Returns raw cycle count (e.g., `return 4;`)
   - WinUAE: Returns `N * CYCLE_UNIT / 2` where `CYCLE_UNIT = 512`.
     Also packs additional info for cycle-exact mode.
   - **Fix:** Either adapt POSE to use CYCLE_UNIT, or modify gencpu to
     emit plain cycle counts.

2. **Memory access function pointers:**
   - WinUAE uses indirection: `x_get_word`, `x_put_word`, `x_prefetch`
     are function pointers set at init time based on CPU mode.
   - POSE uses direct macros: `get_word` → `EmMemGet16`.
   - **Fix:** For simple-cycles 68000 mode, these can be #defined to
     POSE's existing macros. No runtime indirection needed.

3. **Includes and headers:**
   - WinUAE: `sysconfig.h`, `sysdeps.h`, `options.h`, `memory.h`,
     `custom.h`, `events.h`, `cpu_prefetch.h`, `newcpu.h`
   - POSE: `UAE.h` (aggregate header), `EmCommon.h`, PALM-specific headers
   - **Fix:** Create a compatibility shim header that maps WinUAE includes
     to POSE equivalents.

4. **`newcpu.h` / `regstruct` expansion:**
   - WinUAE's `regstruct` has ~100 fields vs POSE's ~20.
   - **Fix:** Add the fields WinUAE's generated code references, or
     stub them out for 68000-only mode.

5. **`do_cycles()` / event system:**
   - WinUAE calls `do_cycles(cpu_cycles)` after each instruction to
     advance the Amiga chipset event scheduler.
   - POSE doesn't have this — it uses `fCycleCount` and `EmHAL::Cycle()`.
   - **Fix:** `do_cycles()` can be a no-op or mapped to our cycle
     accumulation. The generated code just needs the function to exist.

6. **PALM-specific hooks:**
   - POSE's `gencpu.c` has `PALM_PERF`, `PALM_STACK`, `PALM_SYSTEM_CALL`
     etc. These inject profiling, stack checking, and trap tracking code
     into the generated handlers.
   - WinUAE's `gencpu.cpp` has none of these.
   - **Fix:** Re-add PALM hooks to WinUAE's gencpu, or move the hooks
     to the caller side (in EmCPU68K.cpp's execute loop, not in the
     generated handlers).

7. **`adjust_cycles()` function:**
   - WinUAE calls `adjust_cycles(cpu_cycles)` after each instruction,
     which applies the `cpu_clock_multiplier` setting.
   - **Fix:** Good — we can use this for our own speed adjustment,
     or make it a no-op.

## Recommended Approach

### Option A: Update gencpu only (minimal change)

1. Take WinUAE's `gencpu.cpp` (10,329 lines)
2. Strip everything above 68000 level (68010/020/030/040/060, MMU, CE)
3. Re-add POSE's PALM-specific `#ifdef` blocks
4. Configure for `using_simple_cycles = 1` (no cycle-exact, no prefetch)
5. Regenerate `cpuemu.c` with correct cycle counts
6. Adapt return values (divide by `CYCLE_UNIT/2` or adjust caller)
7. Keep all existing POSE integration code (EmCPU68K.cpp, memory banks,
   exception hooks) as-is

**Estimated effort:** 3-5 days.
**Risk:** Low-medium.  The generated handlers change but the integration
layer stays the same.

### Option B: Take cpuemu_0.cpp directly (less work, more risk)

1. Copy WinUAE's `cpuemu_0.cpp` (64,827 lines of generated 68000 code)
2. Create compatibility headers to satisfy its includes
3. Wire its memory access calls to POSE's EmMemory system
4. Adapt the dispatch table to POSE's `cpufunctbl` format
5. Handle the CYCLE_UNIT return value scaling

**Estimated effort:** 2-3 days.
**Risk:** Medium.  We lose the ability to regenerate from source.  Any
future fix requires re-doing the adaptation.  And the 64K file is for
WinUAE's full 68000 mode which may include Amiga-specific behavior we
don't want.

### Option C: Hybrid — gencpu + POSE hooks (recommended)

1. Fork WinUAE's `gencpu.cpp` into our tree
2. Create a stripped-down "POSE mode" that generates only:
   - 68000 handlers with simple cycle counting
   - No prefetch simulation
   - No cycle-exact bus timing
   - PALM_PERF/PALM_STACK hooks injected where needed
3. Generate a new `cpuemu.c` that drops in as a replacement
4. Return plain cycle counts (not CYCLE_UNIT-scaled)
5. Keep our existing `EmCPU68K.cpp` execute loop unchanged

This gives us:
- Correct MC68000 cycle counts from published timing data
- Ability to regenerate when needed
- All POSE-specific features preserved
- Minimal changes to non-UAE code

**Estimated effort:** 4-6 days.
**Risk:** Low.  We control the generator and can iterate.

## What We Don't Need

These WinUAE features can be excluded entirely:

- **68010/020/030/040/060 support** — Palm uses 68000/DragonBall only
- **Cycle-exact mode** (`using_ce`) — overkill for our use case
- **MMU emulation** (`cpummu*.cpp`) — Palm has no MMU
- **JIT compilation** — not applicable
- **Amiga chipset integration** (`do_cycles` event scheduler)
- **PPC co-processor support**
- **Action Replay / debugger integration**
- **Blitter nasty mode**
- **CPU prefetch queue simulation** — nice-to-have but not required
- **IPL (interrupt priority level) accurate timing** — POSE uses simpler
  interrupt dispatch

## Files to Copy from WinUAE

Minimum set:

| File | Purpose | Modifications needed |
|:---|:---|:---|
| `gencpu.cpp` | Code generator | Strip non-68000, add PALM hooks |
| `readcpu.cpp` | Instruction parser | Minor (compatible) |
| `cpudefs.cpp` | Instruction definitions | Minor (compatible) |
| `build68k.cpp` | Build tool | Minor (compatible) |

Generated output:

| File | Purpose | Notes |
|:---|:---|:---|
| `cpuemu.c` | Opcode handlers | Regenerated from modified gencpu |
| `cpustbl.c` | Dispatch tables | Regenerated |
| `cputbl.h` | Handler declarations | Regenerated |

Files we keep from POSE:

| File | Purpose |
|:---|:---|
| `UAE.h` | Aggregate include header |
| `newcpu.h` | Register struct (extend as needed) |
| `memory_cpu.h` | Memory access macros |
| `machdep_maccess.h` | Byte-order memory access |
| `config.h`, `sysconfig.h`, `sysdeps.h` | Platform config |
| `custom.h` | SPCFLAG definitions |

## Verification Plan

After upgrade:

1. **Cycle count spot-check:** Instrument a few common instructions
   (MOVE.W, ADD.W, JSR, RTS, BRA) and verify return values match
   Motorola MC68000 User Manual Appendix D.

2. **Timer rate test:** Run m500 ROM, verify TIMER1 RATE diagnostic
   shows ~1,666us period at 1x speed (same as currently, but with
   correct fCycleCount accumulation).

3. **Speed baseline test:** Time the Welcome animation at 1x speed.
   Should be close to 16.59s (matching real m500 hardware).

4. **Speed multiplier test:** Verify 0.25x/0.50x/1x ratios remain
   correct (should be unchanged — the throttle math is the same).

5. **ROM compatibility:** Boot Palm III (328), Palm V (EZ), m500 (VZ)
   ROMs.  All should reach the launcher.

6. **Gremlins test:** Run Gremlins at max speed.  Should still function
   (timers fire at a rate PalmOS can handle).

## Conclusion

The WinUAE CPU core upgrade is the right fix for the ~4.78x baseline speed
offset.  The architecture is compatible enough that only the code generator
and its output change.  POSE's memory system, exception handling, profiling,
and timer integration remain untouched.

The recommended path (Option C) gives us correct MC68000 timing with low
risk and preserves our ability to maintain the code going forward.
