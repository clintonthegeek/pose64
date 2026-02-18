# Deterministic Canary — LP64 META_CHECK Diagnostic PRC

## Problem

QtPortPOSE (64-bit port of POSE 3.5) fires false memory access violations
that don't occur on the original 32-bit build. Three violation types:

| Violation | Address examples | Path |
|-----------|-----------------|------|
| Storage heap write | 0x41A72, 0x42F8C, 0x42948 | SRAM `fProtect_SRAMSet` → BusError |
| Low memory read | 0x78 | DRAM META_CHECK → ProbableCause |
| Screen memory read | 0x3B00 | DRAM META_CHECK → ProbableCause |

All reported violations come from built-in OS apps (Launcher 4.1P, Memo Pad
4.0, HotSync 4.0, UIAppShell) that run from RAM. The META_CHECK macro is
supposed to whitelist these via `InRAMOSComponent()`, but it isn't working.

## Purpose

A custom .prc with known, controlled behavior that isolates which part of the
emulator's memory protection system is broken on LP64. Each test produces a
predictable outcome — if the emulator's response differs from what's expected,
the deviation points directly at the bug.

## Test Sequence

The canary app displays a simple form with a status field and runs five tests
in order. Between each test it draws a numbered marker on screen (`T1`, `T2`,
etc.) so violations can be correlated to the exact test that triggered them.

### T1 — Baseline (no memory access)

Just draw the form and sit idle for one event loop cycle.

**Expected:** Zero violations. If a violation fires here, the problem is in
`FrmDrawForm` / `EvtGetEvent` internals — the OS trap dispatch isn't guarding
screen/low-memory access with `CEnableFullAccess`.

### T2 — Deliberate low memory read

Read one byte from address `0x78` (exception vector area, marked
`kLowMemoryBits`).

```c
UInt8 val = *(volatile UInt8 *)0x78;
```

**Expected:** Violation fires (kLowMemAccess). This is user app code reading
protected low memory — META_CHECK should catch it. If it does NOT fire, the
meta-memory marking for low memory is broken or IsPCInRAM returns false for
our code.

### T3 — Deliberate screen buffer read

Read one byte from address `0x3B00` (screen buffer area, marked
`kScreenBits`).

```c
UInt8 val = *(volatile UInt8 *)0x3B00;
```

**Expected:** Violation fires (kScreenAccess). Same reasoning as T2. If it
does NOT fire, screen buffer meta-marking is wrong.

### T4 — DmWrite to storage heap

Create a small database, open it, and call `DmWrite` to write a few bytes.

```c
DmOpenRef db;
LocalID id;
MemHandle h;
UInt8 data[] = {0xCA, 0xFE};

DmCreateDatabase(0, "CanaryTestDB", 'CNRY', 'DATA', false);
id = DmFindDatabase(0, "CanaryTestDB");
db = DmOpenDatabase(0, id, dmModeReadWrite);
h = DmNewRecord(db, &idx, sizeof(data));
p = MemHandleLock(h);
DmWrite(p, 0, data, sizeof(data));
MemHandleUnlock(h);
DmReleaseRecord(db, idx, true);
DmCloseDatabase(db);
DmDeleteDatabase(0, id);
```

**Expected:** Zero violations. `DmWrite` internally writes to the storage heap
but does so through the Memory Manager, which should be wrapped in
`CEnableFullAccess`. If a "storage heap write" violation fires here, the trap
patch / `CEnableFullAccess` guard isn't working on LP64 — that's the bug.

### T5 — WinDrawChars (OS screen write)

Draw a string using the standard Window Manager API.

```c
WinDrawChars("Canary OK", 9, 10, 100);
```

**Expected:** Zero violations. `WinDrawChars` writes to the screen buffer
through OS code. If a "screen memory" violation fires, the OS code path
isn't being recognized by `InRAMOSComponent` or the screen access isn't
guarded.

## Interpreting Results

| T1 | T2 | T3 | T4 | T5 | Diagnosis |
|----|----|----|----|----|-----------|
| clean | fires | fires | clean | clean | Emulator is working correctly — false violations come from a different path |
| violation | — | — | — | — | OS trap dispatch broken: `FrmDrawForm`/`EvtGetEvent` not guarded |
| clean | no fire | no fire | clean | clean | Meta-memory marking is broken: regions not marked with access bits |
| clean | fires | fires | violation | clean | `CEnableFullAccess` not working in MemMgr trap patches |
| clean | fires | fires | clean | violation | `InRAMOSComponent` failing for OS screen-write code |
| clean | no fire | no fire | violation | violation | Multiple LP64 bugs: both marking and guarding broken |

## Build

Standard m68k-palmos-gcc build (same toolchain as ShadowStan):

```bash
cd canary/src && make clean && make
```

Produces `Canary.prc`. Install via drag-and-drop in the emulator's New Session
dialog or via the Install App menu.

## Source Location

`canary/src/canary.c` — single-file PalmOS app, no resources beyond a minimal
form (tFRM 1000).
