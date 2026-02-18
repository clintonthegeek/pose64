# META_CHECK / CEnableFullAccess Latent Bug

## Summary

The original POSE 3.5 has a latent bug: the `META_CHECK` macro in
`MetaMemory.h` does not respect `CEnableFullAccess`, the RAII guard that POSE
uses to mark internal emulator operations as exempt from memory access checks.
This bug was invisible on the original FLTK/32-bit build due to coincidental
execution timing. The Qt6 port's different event loop and threading architecture
changed the timing enough to expose it, producing thousands of false "low
memory access" and "screen memory access" violation dialogs during normal ROM
operation.

## The Access Checking System

POSE protects certain memory regions (exception vectors, screen buffer, system
globals, storage heap) with per-byte metadata bits stored in a parallel
`gRAM_MetaMemory[]` array. When emulated code reads or writes DRAM, the bank
handler calls the `META_CHECK` macro, which inspects these bits and calls
`ProbableCause()` if the access is unauthorized.

POSE also has `CEnableFullAccess`, an RAII guard used by the emulator's own C++
code when it needs to read/write emulated memory for internal purposes:

```cpp
// EmCPU68K::ProcessException — reading interrupt vector table
{
    CEnableFullAccess  munge;  // "Remove blocks on memory access."
    newpc = EmMemGet32 (regs.vbr + 4 * exception);
}
```

`CEnableFullAccess` zeroes `gMemAccessFlags` and increments a static access
count. The static method `CEnableFullAccess::AccessOK()` returns true when
the count is positive.

## The Bug

`META_CHECK` never calls `AccessOK()`. It checks `IsPCInRAM()`,
`IsPCInMemMgr()`, and the per-byte meta bits, but it has no awareness of
`CEnableFullAccess`. This means that when POSE's own C++ code reads emulated
memory through the bank handlers (e.g., `EmMemGet32` inside `ProcessException`
to fetch an interrupt vector), `META_CHECK` fires and evaluates the access as
if it came from emulated 68K code.

The interrupt vector table lives at addresses 0x00–0xFF (low memory), marked
with `kLowMemoryBits = 0x07` (blocks app, system, AND memory manager access).
Every time a hardware interrupt fires, `ProcessException` reads the vector
(e.g., address 0x78 for autovector 6 / system timer). This read goes through
`EmBankDRAM::GetLong` -> `META_CHECK`, which sees `kNoSystemAccess` set and
calls `ProbableCause` -> user-visible violation dialog.

## Why It Was Invisible on FLTK/32-bit

Instrumentation of the 32-bit FLTK POSE proved that address 0x78 IS read on
32-bit — just as frequently as on 64-bit. Out of 20 sampled reads:

```
READ32_0x78: PC=0x101134EA accessOK=0    <-- 1 read: 68K ROM code, no guard
READ32_0x78: PC=0x1002A9A8 accessOK=1    <-- 19 reads: ProcessException, guarded
READ32_0x78: PC=0x1002B5AA accessOK=1
READ32_0x78: PC=0x100257CA accessOK=1
...
```

19 of 20 reads had `accessOK=1` (inside `CEnableFullAccess`). The single
`accessOK=0` read occurred at a point in early boot before `MemKernelInit` had
marked the meta bits, so the bits were still 0x00 (total access) and
`META_CHECK` passed trivially. Zero violations resulted.

The FLTK event loop, single-threaded model, and the specific ordering of boot
operations meant that by the time meta bits were set, all interrupt vector reads
happened to occur inside `CEnableFullAccess` — which META_CHECK ignored, but
which also meant the bits at 0x78 happened to already be cleared by
`HwrSleep`'s headpatch at those moments. The result was a stable equilibrium
where the bug never manifested.

## Why the Qt Port Exposed It

The Qt6 port changed:

- **Event loop**: Qt's `QApplication::exec()` event loop vs FLTK's
  `Fl::run()`, with different timer dispatch and idle processing
- **Threading**: Qt signal/slot connections across threads vs FLTK's
  single-threaded callbacks
- **Emulation loop integration**: Different points where the emulation yields
  to the host event loop

These changes altered the relative timing of:
1. When `MemKernelInit` marks low memory bits as 0x07
2. When `HwrSleep` head/tailpatches temporarily unlock/relock vectors
3. When hardware interrupts fire and `ProcessException` reads vectors

On the Qt build, more interrupt vector reads now occur OUTSIDE the
`CEnableFullAccess` window (as direct 68K instruction execution) or at moments
when the meta bits are 0x07, producing 158,000+ false violation dialogs per
boot session.

## Fix

Add `CEnableFullAccess::AccessOK()` as an early exit at the top of
`META_CHECK`. This is architecturally correct: `CEnableFullAccess` exists
specifically to exempt internal emulator operations from access checks, and
`META_CHECK` should respect it.

In `src/core/MetaMemory.h`:

```cpp
#define META_CHECK(metaAddress, address, op, size, forRead)     \
do {                                                            \
    if (CEnableFullAccess::AccessOK ())                         \
        break;                                                  \
    if (Memory::IsPCInRAM ())                                   \
    {                                                           \
        /* ... existing branch logic unchanged ... */           \
    }                                                           \
    /* ... rest of macro unchanged ... */                       \
} while (0)
```

This single line eliminates all false violations from `ProcessException`,
`EmScreen` scans, and any other POSE C++ code that uses `CEnableFullAccess`
when accessing emulated memory.

### Remaining edge case

A small number of violations (1–2 per boot) may still occur from actual 68K ROM
instructions that read interrupt vectors at a moment when the meta bits are
0x07. These are the same reads that the 32-bit FLTK build avoided only by luck
of timing (the read happened before `MemKernelInit` ran). These can be
addressed separately if they prove disruptive, for example by having the
`MemKernelInit` tailpatch leave the autovector range unlocked for system-level
access, since the ROM legitimately reads these vectors.

## Files Modified

| File | Change |
|------|--------|
| `src/core/MetaMemory.h` | Add `AccessOK()` early exit to `META_CHECK` macro |

## Verification

Instrumented both 32-bit FLTK and 64-bit Qt builds with `READ32_0x78` traces at
the top of `EmBankDRAM::GetLong`. Both platforms read address 0x78 at the same
frequency. The 32-bit build shows `accessOK=1` for 95% of reads. The 64-bit
build showed violations only for `accessOK=0` reads. Adding the `AccessOK()`
check reduced violations from 158,000+ to 0–2 per session.
