# Debug Build Methodology for QtPortPOSE

## Problem

The emulator has two stability issues with the Accurate timer:
1. **CPU spikes to 100%** — sometimes freezes UI completely, sometimes just sluggish
2. **RAM leak at ~200KB/min** — slow, only when running PalmOS apps (not at idle)

Current build is hardcoded release mode (`NDEBUG`, `QT_NO_DEBUG`, no `-g`). This makes
GDB backtraces useless (no symbols, inlined functions) and provides zero leak detection.

## Constraints

**False-positive assertions:** The original POSE has ~1,194 `EmAssert()` calls across 96
files. Many of these fire false positives due to a known bug where the emulator cannot
distinguish OS-level memory access from 3rd-party app access (see
`docs/history/meta-check-accessok-bug.md`). The `Report*Access` preferences are already
defaulted to `false`, but blindly re-enabling all `EmAssert` calls risks assertion storms
from other unchecked assumptions in the original 20-year-old code.

**Strategy:** Keep `NDEBUG` defined (assertions stay off) but add debug symbols, ASAN, and
diagnostic history. Assertions can be enabled selectively later once the primary bugs are
found and fixed.

## Design

### 1. CMake Debug Configuration

Add a `POSE_DEBUG` option to CMakeLists.txt that enables:

| Flag | Purpose |
|------|---------|
| `-g3` | Full debug symbols (includes macro definitions) |
| `-O1` | Light optimization (keeps code readable in GDB but not completely unoptimized — `-O0` makes ASAN 10x slower) |
| `-fno-omit-frame-pointer` | Ensures stack traces are complete |
| `-fsanitize=address` | AddressSanitizer: detects out-of-bounds, use-after-free, leaks |
| `-fsanitize=undefined` | UBSan: detects integer overflow, null deref, alignment issues |

**What we keep:** `NDEBUG` stays defined (assertions remain off). `QT_NO_DEBUG` removed
so Qt's own debug output (`qDebug`, `qWarning`) works.

**What we add:**
- `-D_DEBUG` — enables exception history (512-entry circular buffer) and register history
- Remove `QT_NO_DEBUG` — enables Qt debug output

### 2. Build Workflow

```bash
# Debug build (for diagnosing leaks and CPU spikes)
cd /home/clinton/dev/palmtest/QtPortPOSE/build
cmake ../src -DPOSE_DEBUG=ON
make -j$(nproc)

# Release build (normal usage)
cmake ../src -DPOSE_DEBUG=OFF   # or just omit it
make -j$(nproc)
```

The debug binary is the same `qtpose` in the same build directory — no separate output
paths, no complexity.

### 3. Diagnostic Workflows

#### A. Finding the Memory Leak (ASAN + LeakSanitizer)

```bash
# Run with leak detection enabled
ASAN_OPTIONS="detect_leaks=1,log_path=asan.log" ./src/build/qtpose -psf profile.psf
```

When the program exits (or is killed), ASAN prints every unfreed allocation with a full
stack trace showing the exact file:line where the leaked memory was allocated. At 200KB/min,
even a 5-minute run produces clear signal.

**If ASAN is too slow** (the emulator runs a tight per-instruction loop), fall back to:
```bash
valgrind --tool=massif --time-unit=ms ./src/build/qtpose -psf profile.psf
# Then: ms_print massif.out.<pid> | head -80
```
This profiles heap growth over time without the per-access overhead of full Memcheck.

#### B. Diagnosing CPU Spikes (GDB + perf)

**Step 1: Quick triage with perf** (no recompile needed, works on release binary too)
```bash
# In terminal 1: run the emulator
./src/build/qtpose -psf profile.psf &
PID=$!

# In terminal 2: when CPU spikes, sample it
perf top -p $PID              # live view of hot functions
perf record -p $PID -g sleep 5  # 5-second profile with call graph
perf report                     # analyze
```

This immediately tells you whether the spike is in `Execute()`, `Cycle()`,
`processEvents()`, `usleep()`, or somewhere unexpected.

**Step 2: Interactive GDB break-in** (requires debug symbols)
```bash
gdb --args ./src/build/qtpose -psf profile.psf
(gdb) run
# When CPU spikes, press Ctrl+C
(gdb) thread apply all bt     # see all thread backtraces
(gdb) info locals              # inspect timer state
(gdb) print fTmr1CycleAccum   # check accumulator
(gdb) print fCycleCount        # check cycle counter
```

With `-g3 -O1`, you get readable backtraces with local variables. The key things to
inspect during a CPU spike:
- Is the execution loop spinning without yielding? (check loop counter)
- Is `ExecuteStoppedLoop` failing to sleep? (check `cyclesToNext`)
- Is `CycleSlowly` being called? (check the batch counter)
- Are timer interrupts firing correctly? (check compare/counter values)

**Step 3: Automated watchdog** (optional, add later if needed)
If the spike is intermittent and hard to catch manually, a watchdog thread that monitors
`clock_gettime()` deltas and dumps state when the main loop hasn't yielded for >2 seconds.

#### C. Syscall Analysis (strace)

```bash
# Count syscalls during a CPU spike
strace -c -p $(pidof qtpose) -e trace=clock_nanosleep,nanosleep,select,poll
# Look for: thousands of nanosleep calls = tight loop not sleeping
#           zero nanosleep calls = loop not yielding at all
```

### 4. Enabling Assertions (Phase 2 — After Primary Bugs Fixed)

Once the leak and CPU spike are identified and fixed, we can optionally enable assertions
for deeper validation:

```bash
cmake ../src -DPOSE_DEBUG=ON -DPOSE_ASSERTIONS=ON
```

This removes `NDEBUG`, activating all 1,194 `EmAssert()` calls. Expect some false positives
from the known META_CHECK memory access issue (already mitigated by `Report*Access`
preferences defaulting to `false`). Other assertions may fire legitimately, revealing
latent original POSE bugs that the 32-bit build happened to survive.

### 5. What NOT to Change

- **No Qt logging overhaul.** The existing `fprintf(stderr)` pattern works fine with
  stderr redirection. No need to migrate to `qDebug` categories.
- **No custom watchdog thread yet.** GDB Ctrl+C and `perf top` handle the CPU spike
  diagnosis. A watchdog is only needed if the spike is extremely intermittent.
- **No ThreadSanitizer.** The emulator runs its main execution loop on a single thread
  with Qt event processing. TSAN would add massive overhead for minimal gain.
- **No code coverage instrumentation.** Not relevant to these bugs.

## Files Modified

| File | Change |
|------|--------|
| `src/CMakeLists.txt` | Add `POSE_DEBUG` option, conditional compiler/linker flags |

One file. That's it.

## Verification

After the CMake change, verify:
1. `cmake ../src -DPOSE_DEBUG=ON` configures without errors
2. `make -j$(nproc)` builds without new warnings from ASAN/UBSAN flags
3. Running the binary prints ASAN initialization banner to stderr
4. GDB `bt` shows file:line info for all frames
5. Normal `cmake ../src` (without flag) still produces the release build
