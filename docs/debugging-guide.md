# POSE64 Debugging Guide

Reference for all debug facilities available in the emulator: compile-time options,
runtime tools, and the dormant infrastructure activated by the debug build.

## Quick Start

```bash
# Build with full debug instrumentation
mkdir -p build && cd build
cmake .. -DPOSE_DEBUG=ON
make -j$(nproc)

# Run with leak detection
ASAN_OPTIONS="detect_leaks=1" ./pose64

# Run under GDB for interactive debugging
gdb --args ./pose64
```

## Build Configurations

### CMake Options

| Command | What You Get |
|---------|-------------|
| `cmake ..` | Release build. No symbols, no sanitizers, assertions off, Qt debug off. |
| `cmake .. -DPOSE_DEBUG=ON` | Debug build. Symbols + ASAN + UBSAN + exception/register history. Assertions still off. |
| `cmake .. -DPOSE_DEBUG=ON -DPOSE_ASSERTIONS=ON` | Full debug. Everything above plus all 1,194 `EmAssert()` calls active. Use with caution — see "Assertions" below. |

### What POSE_DEBUG Enables

**Compiler flags:**
- `-g3` — full debug symbols including macro definitions
- `-O1` — light optimization (keeps ASAN performant, GDB readable)
- `-fno-omit-frame-pointer` — complete stack traces in all tools
- `-fsanitize=address` — AddressSanitizer (out-of-bounds, use-after-free, leaks)
- `-fsanitize=undefined` — UBSan (integer overflow, null deref, alignment)

**Preprocessor:**
- `-D_DEBUG` — activates exception history, register history, memory manager validation, graphics validation, and the `gLowMemory` convenience pointer
- `QT_NO_DEBUG` removed — `qDebug()`, `qWarning()`, `qInfo()` produce output

**What stays the same:**
- `NDEBUG` remains defined — `EmAssert()` calls are no-ops (safe default)
- All `Report*Access` preferences remain `false` — no false-positive memory violation dialogs

---

## Sanitizers

### AddressSanitizer (ASAN)

Detects memory errors at runtime: out-of-bounds access, use-after-free, double-free,
and memory leaks.

**Basic usage:**
```bash
./pose64    # ASAN is active just by running the debug binary
```

**With leak detection (prints report on exit):**
```bash
ASAN_OPTIONS="detect_leaks=1" ./pose64
```

**Full diagnostic options:**
```bash
ASAN_OPTIONS="detect_leaks=1:log_path=/tmp/asan:verbosity=1" ./pose64
```
This writes reports to `/tmp/asan.<pid>` instead of stderr.

**What ASAN reports look like:**
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000001234
READ of size 4 at 0x602000001234 thread T0
    #0 0x555555678abc in SomeFunction /path/to/file.cpp:42
    #1 0x555555689def in CallerFunction /path/to/file.cpp:100
    ...
```

The stack trace shows exactly where the bad access happened and the full call chain.

**Leak report (on exit):**
```
==12345==ERROR: LeakSanitizer: detected memory leaks
Direct leak of 2048 byte(s) in 1 object(s) allocated from:
    #0 0x7ffff7a12345 in malloc
    #1 0x555555678abc in AllocatingFunction /path/to/file.cpp:55
    #2 0x555555689def in CallerFunction /path/to/file.cpp:80
```

**Key ASAN_OPTIONS:**

| Option | Default | Description |
|--------|---------|-------------|
| `detect_leaks=1` | 1 on Linux | Report unfreed allocations on exit |
| `log_path=FILE` | stderr | Write reports to file instead of console |
| `halt_on_error=0` | 1 | Continue after first error (useful to collect all issues) |
| `verbosity=1` | 0 | Print ASAN initialization info (confirms it's active) |
| `fast_unwind_on_malloc=0` | 1 | Slower but more complete stack traces for allocations |
| `malloc_context_size=30` | 30 | Stack depth for allocation traces |

### UndefinedBehaviorSanitizer (UBSAN)

Catches undefined behavior: signed integer overflow, null pointer dereference,
misaligned access, shift overflow, etc. Active automatically in the debug binary.

**Reports look like:**
```
file.cpp:42:15: runtime error: signed integer overflow: 2147483647 + 1
    cannot be represented in type 'int'
```

**UBSAN options (via environment variable):**
```bash
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0" ./pose64
```

---

## GDB

The debug build produces full symbols, making GDB useful for both crash analysis and
live debugging of CPU spikes and hangs.

See `docs/gdb-crash-debugging.md` for crash-specific GDB usage (batch mode, reading
backtraces, common patterns).

### Diagnosing CPU Spikes

When the emulator spikes to 100% CPU:

```bash
gdb --args ./pose64
(gdb) handle SIGPIPE nostop noprint pass
(gdb) run
```

When the spike occurs, press **Ctrl+C** to break in:

```
(gdb) thread apply all bt          # Which thread is busy? What's it doing?
(gdb) info locals                   # Local variables in the current frame
```

**Key variables to inspect during a spike:**

```
# In the CPU execution loop (EmCPU68K::Execute)
(gdb) print fCycleCount             # Total cycles executed
(gdb) print gSession->fStopped      # Is the CPU in STOP state?

# Timer state (for EmRegsVZ / m500)
(gdb) frame N                       # Navigate to the timer frame
(gdb) print fTmr1CycleAccum         # Bresenham accumulator value
(gdb) print fAccurateTimers          # Is accurate timer mode on?

# Qt event loop health
(gdb) print fNeedIdle                # Does the app think it needs idle processing?
```

**Watchpoints** (break when a variable changes — useful for finding what triggers a spike):

```
(gdb) watch fTmr1CycleAccum         # Break when accumulator changes
(gdb) rwatch fCycleCount             # Break when cycle count is read
(gdb) awatch gSession->fStopped      # Break on any access to fStopped
```

### Inspecting Debug History Buffers

The debug build activates 512-entry circular buffers for register and exception history.

**Register history** (every M68K instruction's full register state):
```
(gdb) print gCPU68K->fRegHistoryIndex           # Current position in buffer
(gdb) print gCPU68K->fRegHistory[0]              # Oldest entry (if wrapped)

# The most recent entry:
(gdb) set $idx = (gCPU68K->fRegHistoryIndex - 1) & 511
(gdb) print gCPU68K->fRegHistory[$idx]

# Walk back through recent history:
(gdb) set $i = 0
(gdb) while $i < 10
 > set $idx = (gCPU68K->fRegHistoryIndex - 1 - $i) & 511
 > printf "-%d: PC=0x%08x\n", $i, gCPU68K->fRegHistory[$idx].pc
 > set $i = $i + 1
 > end
```

**Exception history** (last 512 exceptions with PC and SP):
```
(gdb) print gCPU68K->fExceptionHistoryIndex      # Current position

# Most recent exception:
(gdb) set $idx = (gCPU68K->fExceptionHistoryIndex - 1) & 511
(gdb) print gCPU68K->fExceptionHistory[$idx]
# Shows: { name = "Timer", pc = 0x10abc, sp = 0x1ff00 }

# List last 20 exceptions:
(gdb) set $i = 0
(gdb) while $i < 20
 > set $idx = (gCPU68K->fExceptionHistoryIndex - 1 - $i) & 511
 > printf "-%d: %s  PC=0x%08x  SP=0x%08x\n", $i, gCPU68K->fExceptionHistory[$idx].name, gCPU68K->fExceptionHistory[$idx].pc, gCPU68K->fExceptionHistory[$idx].sp
 > set $i = $i + 1
 > end
```

**Low memory globals** (only in debug build):
```
(gdb) print *gLowMemory               # Full PalmOS low-memory header
```

---

## perf (Linux Performance Profiling)

Works on both debug and release binaries. No recompile needed.

### Quick CPU Spike Triage

```bash
# Start the emulator
./pose64 &

# When CPU spikes, see where time is being spent (live):
perf top -p $(pidof pose64)
```

This shows a live-updating list of functions sorted by CPU time. Look for:
- `EmCPU68K::Execute` dominating = tight emulation loop not yielding
- `EmRegsVZ::Cycle` dominating = timer advancement loop
- `clock_nanosleep` / `usleep` = expected idle behavior (not a spike)
- `QApplication::processEvents` = Qt event processing

### Recorded Profile (with call graph)

```bash
# Record 5 seconds of samples during a spike
perf record -p $(pidof pose64) -g --call-graph dwarf sleep 5

# Analyze
perf report
```

With the debug build's `-fno-omit-frame-pointer`, call graphs are complete.

---

## strace (Syscall Analysis)

Useful for understanding whether the CPU spike is userspace computation or
excessive kernel calls.

```bash
# Count syscalls over 5 seconds during a spike
strace -c -p $(pidof pose64) 2>&1 & sleep 5; kill %1

# Or trace specific syscalls:
strace -p $(pidof pose64) -e trace=clock_nanosleep,nanosleep,write
```

**What to look for:**

| Pattern | Meaning |
|---------|---------|
| Thousands of `clock_nanosleep` calls | Tight loop sleeping for tiny intervals — not actually yielding |
| Zero `clock_nanosleep` calls | Loop not yielding at all (pure CPU spin) |
| Many `write` calls to stderr | Logging or ASAN output flooding |
| `mmap` / `brk` calls accumulating | Active memory allocation (correlates with leak) |

---

## Valgrind (Alternative Memory Analysis)

If ASAN is too intrusive for a specific scenario (e.g., timing-sensitive bugs that
disappear under ASAN's overhead), Valgrind's Massif tool profiles heap growth with
less per-access overhead.

### Heap Profiling with Massif

```bash
# Profile heap allocations over time
valgrind --tool=massif --time-unit=ms ./pose64

# After exit, analyze the profile:
ms_print massif.out.$(pidof pose64) | head -80
```

This shows a timeline graph of heap usage, identifying which allocation sites
are growing.

### Full Memory Check (slow)

```bash
# 10-50x slower, but finds every memory error
valgrind --tool=memcheck --leak-check=full --track-origins=yes ./pose64
```

Only use this if ASAN misses something or you need `--track-origins` to find
where an uninitialized value came from.

---

## Built-In Logging System

The emulator has a full logging system (`Logging.h`) with 19 log categories and 18
memory-access report categories. These are controlled by the emulator's preference
system and work in both release and debug builds.

### Log Categories

| Category | What It Logs |
|----------|-------------|
| `LogErrorMessages` | Emulator error messages |
| `LogWarningMessages` | Warning messages |
| `LogGremlins` | Gremlin (fuzz testing) events |
| `LogCPUOpcodes` | Every M68K instruction executed (extremely verbose) |
| `LogEnqueuedEvents` | Events added to PalmOS event queue |
| `LogDequeuedEvents` | Events removed from queue |
| `LogSystemCalls` | PalmOS system trap calls |
| `LogApplicationCalls` | App-level calls |
| `LogSerial` / `LogSerialData` | Serial I/O and data hex dumps |
| `LogNetLib` / `LogNetLibData` | Network library calls and data |
| `LogExgMgr` / `LogExgMgrData` | Exchange manager activity |
| `LogHLDebugger` / `LogHLDebuggerData` | High-level debugger protocol |
| `LogLLDebugger` / `LogLLDebuggerData` | Low-level debugger protocol |
| `LogRPC` / `LogRPCData` | Remote procedure calls |

### Memory Access Reports

These are all defaulted to `false` to avoid false positives from a known POSE bug
where the emulator cannot distinguish OS memory access from 3rd-party app access
(see `docs/history/meta-check-accessok-bug.md`).

| Category | What It Reports |
|----------|----------------|
| `ReportLowMemoryAccess` | Access to exception vectors (0x00-0xFF) |
| `ReportSystemGlobalAccess` | Access to system globals (0x102-0x38E) |
| `ReportScreenAccess` | Direct screen buffer access |
| `ReportHardwareRegisterAccess` | Hardware register access |
| `ReportFreeChunkAccess` | Access to freed heap memory |
| `ReportUnlockedChunkAccess` | Access to unlocked heap handles |
| `ReportMemMgrDataAccess` | Memory manager internal structures |
| `ReportLowStackAccess` | Stack overflow detection |
| `ReportMemMgrLeaks` | Memory manager leak detection |

### Using Logging

Logging preferences are controlled through the emulator's preference system. Log
output goes to timestamped files (`Log_0001.txt`, `Log_0002.txt`, etc.) in the
emulator's working directory.

The logging macros are available throughout the codebase:
```cpp
LogAppendMsg("Timer fired: counter=%d, compare=%d", counter, compare);
LogAppendData(buffer, length, "Packet data");
LogDump();  // Flush buffer to file
```

---

## Assertions (Phase 2)

The emulator contains 1,194 `EmAssert()` calls across 96 files. These are disabled
by default even in the debug build (`NDEBUG` stays defined) because:

1. The original POSE codebase has false-positive assertions from known bugs
2. The 32-to-64-bit port may trigger assertions in previously dead code paths
3. Some assertions validate assumptions that were true on 32-bit but not on 64-bit

### Enabling Assertions

```bash
cmake .. -DPOSE_DEBUG=ON -DPOSE_ASSERTIONS=ON
make -j$(nproc)
```

This removes `NDEBUG`, activating all `EmAssert()` calls. When an assertion fails:
1. `MyAssertFailed()` is called, which dumps the log buffer to file
2. `Platform::Debugger()` is called, which raises `SIGTRAP`
3. If running under GDB, you get a breakpoint at the assertion site

### What _DEBUG Activates (Beyond Assertions)

These are always active in the POSE_DEBUG build regardless of POSE_ASSERTIONS:

| Feature | Location | Description |
|---------|----------|-------------|
| Register history | `EmCPU68K.h:325` | 512-entry circular buffer of full M68K register snapshots, one per instruction |
| Exception history | `EmCPU68K.h:331` | 512-entry buffer of exception name + PC + SP |
| MemMgr validation | `EmPatchModuleMemMgr.cpp:1238,1346` | Checks for duplicate allocations and stale handles |
| `gLowMemory` pointer | `EmBankSRAM.cpp:65` | Global pointer to PalmOS low-memory globals for GDB inspection |
| Graphics ROP validation | `EmRegsMediaQ11xx.cpp:1576,3073` | Double-checks raster operation calculations |

---

## Workflow: Hunting a Memory Leak

1. Build with `POSE_DEBUG=ON`
2. Run: `ASAN_OPTIONS="detect_leaks=1:halt_on_error=0" ./pose64`
3. Exercise the scenario that triggers the leak (run PalmOS apps)
4. Exit the emulator (close window or Ctrl+C)
5. ASAN prints every unfreed allocation with full stack traces
6. The stack trace shows file:line of the `malloc`/`new` that leaked

## Workflow: Hunting a CPU Spike

1. Build with `POSE_DEBUG=ON`
2. Start: `gdb --args ./pose64`
3. `(gdb) handle SIGPIPE nostop noprint pass`
4. `(gdb) run`
5. When the spike occurs, press **Ctrl+C**
6. `(gdb) thread apply all bt` — find the busy thread
7. `(gdb) info locals` — inspect loop counters and timer state
8. If the spike is in the emulation loop, check the exception history to see
   what the emulated CPU was doing leading up to it
9. Optional: in another terminal, `perf top -p $(pidof pose64)` for a
   function-level heat map

## Workflow: Switching Between Debug and Release

```bash
# Switch to debug
cmake .. -DPOSE_DEBUG=ON && make -j$(nproc)

# Switch to release
cmake .. -DPOSE_DEBUG=OFF && make -j$(nproc)

# Full rebuild (if switching seems stale)
cmake .. -DPOSE_DEBUG=ON && make clean && make -j$(nproc)
```

The binary is always `build/pose64` — no separate output paths.
