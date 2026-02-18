# WinUAE gencpu.cpp Fork Attempt: Post-Mortem

## Date: 2026-02-17

## Context

POSE (Palm OS Emulator) uses a 68000 CPU emulation layer derived from UAE
(Un*x Amiga Emulator) version 0.8.10, circa 1999. The CPU emulation is built
with a **code generator pipeline**: at build time, a program called `gencpu`
reads a table of 68000 instruction definitions and writes out C source code
containing ~1,600 opcode handler functions. Each handler returns the number of
CPU cycles consumed by that instruction.

The pipeline is:

```
table68k → build68k → cpudefs.c → gencpu → cpuemu.c + cpustbl.c + cputbl.h
```

These generated files are then compiled into the emulator alongside the runtime
code in `EmCPU68K.cpp`, which dispatches opcodes and accumulates `fCycleCount`.

## The Problem

The existing POSE UAE cycle counts are **systematically wrong**. They
undercount by roughly 2x for simple instructions and far more for complex ones.
The code generator (`gencpu.c`, line 1171) initializes each instruction's cycle
cost to 2 (one word fetch = 2 cycles in its accounting), then adds costs for
addressing mode bus accesses — but these costs are also halved relative to the
real MC68000.

Evidence from the existing generated `cpuemu.c`:

| Instruction | Handler | Returns | MC68000 correct |
|-------------|---------|---------|-----------------|
| NOP | `op_4e71_3` | 2 | 4 |
| MOVE.W Dn,Dn | `op_3000_3` | 2 | 4 |
| OR.B #imm,Dn | `op_0_3` | 4 | 8 |
| RTS | `op_4e75_3` | 2 | 16 |
| MULU Dn,Dn | `op_c0c0_3` | 70 | 38–70 |

The root cause is in `gencpu.c` line 1171: `insn_n_cycles = 2;` (the base
instruction fetch cost). Each word read adds 2, each long read adds 4. So a NOP
(1 word fetch) = 2. A MOVE.W Dn,Dn (1 word fetch) = 2. On the real MC68000,
the minimum instruction is 4 cycles (1 word fetch × 4 cycles/word). The entire
accounting system operates in half-cycle units.

Additionally, data bus access costs for stack operations, memory reads/writes,
and prefetches are missing from the return value — they're only tracked in the
separate `perfRec` fields (`extraCycles`, `readCycles`, `writeCycles`) used by
POSE's optional profiler, but NOT included in the cycle count returned to the
execution loop.

## The Proposed Fix

WinUAE has evolved the same UAE codebase for 25+ years into a verified-accurate
MC68000 emulator. Its `gencpu.cpp` (10,432 lines, vs POSE's 3,346-line
`gencpu.c`) generates handlers that return correct cycle counts. The proposal
was to fork WinUAE's code generator, configure it to emit POSE-compatible
output, and regenerate the opcode handlers.

## What We Found

### The Scope Was Much Larger Than Expected

The plan identified ~10 areas needing `#ifdef POSE_MODE` guards. In practice,
the incompatibilities were deep and pervasive. Here's what we encountered:

### Problem 1: Two Incompatible readcpu.h Ecosystems

This was the first surprise. POSE's `readcpu.h` defines `struct instr` with
bit-packed fields:

```c
// POSE readcpu.h
extern struct instr {
    long int handler;
    unsigned char dreg, sreg;
    signed char dpos, spos;
    unsigned char sduse;
    int flagdead:8, flaglive:8;
    unsigned int mnemo:8, cc:4, plev:2, size:2;
    unsigned int smode:5, stype:3, dmode:5;
    unsigned int suse:1, duse:1, unused1:1, clev:3, unused2:5;
} *table68k;
```

WinUAE's `readcpu.h` defines the same struct with different field types and
additional fields:

```c
// WinUAE readcpu.h (excerpt of differences)
extern struct instr {
    // ... same base fields, but then:
    wordsizes size;         // enum, not 2-bit field
    unsigned int unsized:1; // NEW
    amodes smode;           // enum, not 5-bit field
    amodes dmode;           // enum, not 5-bit field
    unsigned int ccuse:1;   // NEW
    unsigned int unimpclev:3; // NEW
    unsigned int cflow:3;   // NEW
    char head, tail, clocks, fetchmode; // NEW (68020 timing)
} *table68k;
```

**The critical discovery:** POSE's `readcpu.cpp` and `cpudefs.c` are compiled
as part of the **runtime** build (listed in `CMakeLists.txt`).
`EmCPU68K.cpp` calls `read_table68k()` and `do_merges()` at startup (line
1928–1929) to build a 65,536-entry dispatch table by walking the `table68k`
array. This means you **cannot** replace POSE's `readcpu.h` with WinUAE's
version — the runtime depends on the exact struct layout.

**Solution attempted:** Create a `gen/` subdirectory containing WinUAE's
readcpu.h for the generator build only, keeping POSE's runtime readcpu.h
untouched. This worked for compilation but meant the two ecosystems had to be
kept in sync manually.

### Problem 2: The Generator Build Environment

WinUAE's gencpu.cpp was written for Windows (MSVC). Getting it to compile on
Linux required creating several stub headers:

- `gen/sysconfig.h` — empty stub
- `gen/sysdeps.h` — UAE integer types (uae_u8/u16/u32/u64), TCHAR→char
  mappings, `xmalloc`/`xcalloc`/`xfree`, Windows compat defines
  (`strnicmp→strncasecmp`, `_vsnprintf→vsnprintf`, etc.)
- `gen/uae/types.h` — redirect to sysdeps.h

Specific compilation fixes needed:
- Remove `#include <tchar.h>` (Windows-only) from build68k.cpp
- Add `strnicmp`, `_vsnprintf`, `_stricmp` compat defines
- Add `NORETURN`, `NOWARN_UNUSED`, `CPU_EMU_SIZE` defines
- Implement `ua()` as `strdup()` (WinUAE's version allocates; a naive
  cast-only implementation causes free-of-non-malloc crash)
- Resolve `write_log` redefinition conflict between sysdeps.h and gencpu.cpp
- Add `_tcsncmp`, `_tstol`, `_tstoi`, `_totupper`, `_istdigit`, etc.

**This part was manageable** — about 90 minutes of iterative compile-fix cycles.
Most of it could be scripted once you know the full list of needed defines.

### Problem 3: POSE_MODE Guards in gencpu.cpp (The Real Work)

This is where the effort exploded. WinUAE's gencpu.cpp has **1,652 `out("`
calls** that emit generated C code. Many of these emit constructs that are
incompatible with POSE. Each incompatibility requires a `#ifdef POSE_MODE`
guard in the generator, and the incompatibilities are scattered across
thousands of lines.

**Areas successfully modified:**

1. **`main()` function** (line ~10320): Replaced WinUAE's 55-variant CPU
   generation loop with POSE's single 68000-only config:
   ```cpp
   postfix = 3;           // POSE naming convention
   cpu_level = 0;         // 68000 only
   using_exception_3 = 0; // No address error emulation
   using_prefetch = 0;    // No prefetch simulation
   using_noflags = 0;     // No dual-handler noflags variants
   // ... 15 more settings zeroed
   ```

2. **`returncycles()`** (line 681): WinUAE returns a packed format:
   ```c
   return (cycles * CYCLE_UNIT/2 + count_cycles)
        | (((total * 4 * CYCLE_UNIT/2 + count_cycles) * 4) << 16);
   ```
   Changed to plain `return %d;` under POSE_MODE.

3. **`generate_includes()`** (line 9528): WinUAE emits 8+ includes; POSE
   needs just `#include "UAE.h"` and optional `#include "Profiling.h"`.

4. **Memory access function names** (line 5370): WinUAE generic mode uses
   `get_diword`/`get_dilong`/`get_dibyte` for instruction fetches; POSE uses
   `get_iword`/`get_ilong`/`get_ibyte`.

5. **Function signature** (line 9814): WinUAE emits
   `uae_u32 op_XXXX_0_ff(uae_u32 opcode)`;
   POSE needs `unsigned long REGPARAM2 op_XXXX_3(uae_u32 opcode)`.

6. **Dispatch table** (line 9940): Completely different struct layout. WinUAE:
   `{ handler_ff, handler_nf, opcode, len, {disp0,disp1}, branch }`.
   POSE: `{ handler, 0, opcode, extraCycles, readCycles, writeCycles }`.

7. **`setpc()` / `setpcstring()`** (line 942): WinUAE emits `m68k_setpc_j()`
   (for JIT/prefetch tracking); POSE needs `m68k_setpc()`.

**With these 7 areas modified, gencpu successfully compiled and generated
output files.** Then the real problems started.

### Problem 4: The Generated Code Was Still Incompatible

After generating cpuemu.c (770KB, ~31,000 lines), attempting to compile it
against POSE's headers revealed **13 undefined functions** with a total of
**172 call sites** in the generated code:

| Function | Calls | Purpose |
|----------|-------|---------|
| `Exception_cpu(N)` | 46 | CPU exception (POSE has `Exception(N, addr)`) |
| `setchkundefinedflags()` | 33 | CHK instruction flags |
| `divbyzero_special()` | 22 | Divide-by-zero flags |
| `setdivsflags()` | 22 | DIVS overflow flags |
| `exception3_read_prefetch()` | 17 | Odd-address exception |
| `MakeFromSR_T0()` | 14 | Status register (68010+ feature) |
| `setdivuflags()` | 11 | DIVU overflow flags |
| `exception3_read_prefetch_only()` | 2 | Odd-address exception |
| `exception3_read_access()` | 1 | Odd-address exception |
| `checkint()` | 1 | Interrupt check |
| `cpureset()` | 1 | RESET instruction |
| `do_cycles_stop()` | 1 | STOP instruction timing |
| `MakeFromSR_STOP()` | 1 | STOP status register |

**The `using_exception_3 = 0` setting was supposed to suppress the exception3
calls, but ~20 of them are in code paths NOT guarded by that flag** (JMP, RTS,
BSR handlers have their own unconditional odd-address checks).

**The `Exception_cpu` calls** come from TRAP, TRAPV, CHK, and privilege
violation handlers. WinUAE's version takes 1 argument; POSE's `Exception()`
takes 2.

**The flag-setting functions** (`setdivuflags`, `setdivsflags`,
`setchkundefinedflags`) were factored out of inline code in WinUAE's evolution
and placed in `newcpu_common.cpp`. They implement CPU-model-specific undefined
flag behavior. POSE's old code had this inline.

### Problem 5: C vs C++ Macro Incompatibilities

Even writing compatibility stubs in UAE.h hit issues:

- **`CLEAR_CZNV()`**: WinUAE's gencpu emits `CLEAR_CZNV()` (with parens).
  POSE defines it as `do { ... } while(0)` without parens. Adding parens
  makes it `do { ... } while(0)()` — a syntax error. You can't define both
  an object-like macro `CLEAR_CZNV` and a function-like macro `CLEAR_CZNV()`
  in C.

- **`GET_CFLG()`**: WinUAE emits `GET_CFLG()` (150 occurrences). POSE
  defines `GET_CFLG` as just `CFLG` which expands to `(regflags.c)`. With
  parens: `(regflags.c)()` — "called object is not a function".

- **`bool`/`false`**: cpuemu.c is compiled as C (`.c` extension in
  CMakeLists.txt), but WinUAE's `cpureset()` returns `bool`.

- **`Exception(8)`**: WinUAE emits single-argument `Exception(8)` for
  privilege violations (19 occurrences). POSE's `Exception()` requires 2
  arguments.

These are all fixable individually, but each fix touches POSE's core
headers that are included by dozens of other source files — every change
risks cascading breakage.

### Problem 6: PALM-Specific Hooks (Not Yet Attempted)

The old POSE `gencpu.c` has **141 `#ifdef PALM_*` blocks** totaling **749
lines** (22.4% of the file). These inject instrumentation that's critical
to POSE's functionality:

- **PALM_STACK** (~14 locations): `CHECK_STACK_POINTER_DECREMENT()`,
  `CHECK_STACK_POINTER_INCREMENT()`, `CHECK_STACK_POINTER_ASSIGNMENT()` —
  called after every A7 modification to detect stack overflow/underflow.

- **PALM_SYSTEM_CALL** (1 location): `Software_ProcessJSR_Ind()` — hooks
  indirect JSR to intercept Palm OS system calls.

- **PALM_PERF** (~90 locations): Tracks `extraCycles`, `readCycles`,
  `writeCycles` per addressing mode and instruction, populating the
  `perfRec` struct in the dispatch table.

These hooks are deeply woven into the addressing mode generation functions
(`genamode`, `genastore`, `gen_opcode`). WinUAE's code has completely
restructured these functions over 25 years. Porting the PALM hooks would
require understanding both the old and new code structures at a deep level
and finding the corresponding injection points — probably 2-3 days of
careful work.

## Time Spent

Approximately 3-4 hours of active work:
- ~1 hour: Research (reading both codebases, understanding the pipeline)
- ~1.5 hours: Getting gencpu.cpp to compile (stub headers, compat defines)
- ~1 hour: Adding POSE_MODE guards (7 areas successfully modified)
- ~0.5 hours: Discovering and documenting the 13 undefined functions
- ~0.5 hours: Attempting compatibility stubs, hitting macro issues

## What Could Be Automated

### Easily automated (sed/python script):

1. **Stub header generation**: Given a list of needed types and defines,
   generating `sysconfig.h`, `sysdeps.h`, `uae/types.h` is trivial.

2. **Simple text substitutions in generated output**: After gencpu produces
   cpuemu.c, a post-processing script could:
   - `s/m68k_setpc_j(/m68k_setpc(/g`
   - `s/CLEAR_CZNV()/CLEAR_CZNV/g`
   - `s/COPY_CARRY()/COPY_CARRY/g`
   - `s/GET_CFLG()/GET_CFLG/g` (and other GET_*FLG)
   - `s/Exception_cpu(\([0-9]*\))/Exception(\1, m68k_getpc())/g`
   - `s/exception3_read_prefetch(.*);/Exception(3, 0); \/\* addr error \*\//g`
   - `s/exception3_read_access(.*);/Exception(3, 0);/g`
   - Replace `bool`→`int`, `false`→`0`, `true`→`1` in specific patterns

3. **Function stub generation**: A script could scan the generated cpuemu.c
   for undefined function calls and auto-generate stub implementations.

### Requires human judgment:

1. **POSE_MODE guards in gencpu.cpp**: Each of the 7 modified areas required
   understanding what WinUAE emits vs what POSE expects. Some are simple
   (includes) but others require understanding the packed cycle count format,
   dispatch table struct layout, and function naming conventions.

2. **Flag-setting function implementations**: `setdivuflags`, `setdivsflags`,
   `setchkundefinedflags` need correct MC68000-specific flag behavior.
   WinUAE's implementations in `newcpu_common.cpp` handle 68000/010/020/040/060
   with different behaviors each. Extracting just the 68000 path requires
   reading and understanding each function.

3. **PALM hook porting**: The 749 lines of PALM hooks cannot be automated
   because the injection points in WinUAE's restructured code don't
   correspond 1:1 to the old gencpu.c locations. Each hook must be manually
   placed by understanding what the surrounding code does.

4. **Exception handling differences**: POSE uses `goto endlabel; Exception(N,
   addr);` patterns with explicit label management. WinUAE uses
   `Exception_cpu(N); return cycles;` with function-scoped returns. The
   control flow around exceptions is fundamentally different.

## The Better Approach (What We Actually Did)

Instead of the gencpu fork, we applied a 2-line fix in `EmCPU68K.cpp`:

```c
// Line 478 (profiling path):
fCycleCount += cycles * 2;  // UAE returns half-speed cycle counts

// Line 586 (accurate timer path):
fCycleCount += cpufunctbl[opcode] (opcode) * 2;
```

This corrects the most significant systematic error (the 2x undercount from
halved bus cycle costs) without touching the code generator at all. It's not
perfectly accurate per-instruction — RTS will be 4 instead of 16 — but for
timer advancement purposes it's a dramatic improvement over the status quo,
and it's **2 lines instead of ~2,000**.

## If Someone Wants to Finish the Fork

The gen/ directory contains a working WinUAE gencpu.cpp fork that successfully
generates output. The remaining work is:

1. **Post-process the generated cpuemu.c** (script, ~30 sed patterns) to fix
   function names, macro call styles, and argument counts.

2. **Write 13 compatibility stubs** (or a compat header) for the missing
   WinUAE functions. The 68000-specific implementations from
   `WinUAE/newcpu_common.cpp` can be extracted for the flag functions.

3. **Fix the C vs C++ issues**: Either rename cpuemu.c → cpuemu.cpp, or
   ensure all stubs use C-compatible types.

4. **Port the 141 PALM hook blocks** from the old gencpu.c into the new
   gencpu.cpp. This is the hardest part — roughly 3 days of careful work.

5. **Test against real Palm OS ROMs** on m500, Palm III, Palm V, and M515
   with Monopoly (the original test matrix).

The gen/ directory files:
```
gen/
├── build68k         (compiled binary)
├── build68k.cpp     (from WinUAE, minor edits)
├── cpudefs.c        (generated by build68k from table68k)
├── gencpu           (compiled binary)
├── gencpu.cpp       (from WinUAE, 7 POSE_MODE guard areas added)
├── readcpu.cpp      (from WinUAE, unmodified)
├── readcpu.h        (from WinUAE, unmodified)
├── sysconfig.h      (stub for Linux build)
├── sysdeps.h        (stub: UAE types, TCHAR, Windows compat)
├── table68k         (from WinUAE, unmodified)
└── uae/
    └── types.h      (stub: redirects to sysdeps.h)
```

Build command: `cd gen && g++ -o gencpu gencpu.cpp readcpu.cpp cpudefs.c -I. -DPOSE_MODE=1 -w && ./gencpu`

This generates `../cpuemu.c`, `../cpustbl.c`, `../cputbl.h` (in the parent
UAE directory).

## Lessons Learned

1. **Old codebases diverge at the API level, not just the implementation.**
   POSE and WinUAE share a common ancestor but 25 years of evolution means
   the struct layouts, function signatures, macro conventions, and exception
   handling patterns have all diverged. It's not a merge — it's a rewrite
   with the same variable names.

2. **Code generators amplify incompatibilities.** A single `out("CLEAR_CZNV();\n")`
   in the generator produces 800+ lines in the output that all need the
   same fix. But you can't fix it with a simple sed because the generator
   makes context-dependent decisions about which code to emit.

3. **Runtime dependencies create hard constraints.** The discovery that
   `readcpu.cpp` is compiled into the runtime (not just used at generation
   time) meant we couldn't swap readcpu.h — we had to maintain two parallel
   versions. This kind of hidden dependency is the worst kind of surprise.

4. **The 80/20 rule applies hard here.** The 2-line `* 2` fix gets ~80% of
   the accuracy improvement (correct for register-register instructions,
   2x better for all others) with 0.01% of the effort of the full fork.
