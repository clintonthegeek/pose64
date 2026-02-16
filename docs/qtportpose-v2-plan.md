# QtPortPOSE v2: Complete Reimplementation Plan

## Context

QtPortPOSE v1 was an attempt to port POSE (Palm OS Emulator) 3.5 from FLTK to Qt6 on 64-bit Linux. It failed due to:

1. **Bypassed POSE lifecycle** -- `EmApplication::Startup()` never called; no `EmApplicationQt` subclass; window ownership inverted; `Shutdown()` never called. This silently disabled socket infrastructure, debugger support, RPC, logging, and clipboard handling.

2. **Broken 64-bit types** -- The fundamental type `uint32` is `typedef unsigned long` which is **8 bytes on LP64**. Combined with suppressed cast warnings (`-Wno-int-to-pointer-cast`), this created silent data corruption throughout the codebase.

3. **Broken threading** -- A 3-thread model (UI + bridge + CPU) that broke `omni_thread::self()` identity checks and introduced deadlock potential via competing `EmSessionStopper` calls.

4. **Half-finished implementation** -- 25+ dialog stubs, zero keyboard input, no ReControl API, visual feedback methods that do nothing.

Separately, RePOSE4's ReControl socket API (added to the FLTK build) exposed deep session state bugs: suspend counter orphaning, unrecoverable suspension after app crashes, intermittent bus errors during install, and `BlockOnDialog` creating dead states unreachable from the socket.

This plan addresses all of these failures with a staged, correctness-first approach.

---

## Phase 1: Foundation -- Type System Fix and Project Structure

### 1.1 Rename `src/` to `abandoned/`
Move the failed v1 source out of the way for reference.

### 1.2 Create new project structure
```
src/
  CMakeLists.txt
  core/           # Original POSE 3.5 source (from Emulator_Src_3.5/SrcShared/)
    UAE/          # m68k CPU emulator
    Hardware/     # Device emulation
    Patches/      # ROM patches
    Palm/         # SDK headers
    omnithread/   # Threading primitives
  platform/       # Qt platform implementations (new files we write)
  ui/             # Qt UI layer (new files we write)
```

### 1.3 Fix the type system (FIRST PRIORITY)

**The root bug**: `EmTypes.h` line 29-30:
```cpp
typedef signed long    int32;   // 8 bytes on LP64!
typedef unsigned long  uint32;  // 8 bytes on LP64!
```

**Fix**: Change to fixed-width types:
```cpp
#include <cstdint>
typedef int32_t   int32;
typedef uint32_t  uint32;
```

This single change makes `emuptr` (which is `typedef uint32 emuptr`) genuinely 32-bit, which turns silent truncation into compile errors at every problematic cast site.

**File**: `core/EmTypes.h`

### 1.4 Fix all resulting compile errors

With `uint32` now truly 32 bits, every `emuptr ↔ host pointer` cast will fail to compile. These must be fixed individually:

- **EmMemory.h** (`EmMemGetRealAddress`, `EmMemDoGet8/Put8`): These correctly return `uint8*` host pointers from `emuptr` via bank translation. The `xlateaddr` function performs the actual lookup. No truncation here -- just ensure `(long) a ^ 1` in `EmMemDoGet8` uses `uintptr_t`.

- **UAE core** (`newcpu.h`): `regs.pc_p` is `uae_u8*` (host pointer) and `regs.pc` is `uae_u32` (emulated address). These are **correctly separate types**. Ensure no casts between them; use `EmMemGetRealAddress(pc)` to convert.

- **Marshal.cpp**: ~80 sites casting `emuptr` to host pointers for parameter marshalling. Each needs conversion to use `EmMemGetRealAddress()`.

- **ChunkFile/SessionFile**: Serialization code that may store pointer values. Must audit to ensure only `emuptr` values (32-bit emulated addresses) are serialized, never host pointers.

- **HostControl.cpp**: ~150 handler functions marshalling between host and emulated memory. Already uses `EmSubroutine` for safe calls -- verify no raw casts.

**Strategy**: Enable warnings as errors for pointer-int casts (`-Werror=int-to-pointer-cast -Werror=pointer-to-int-cast`). Fix each error. Never suppress these warnings.

---

## Phase 2: Application Lifecycle -- EmApplicationQt

### 2.1 Create `EmApplicationQt` subclass

Following `EmApplicationFltk` (`Emulator_Src_3.5/SrcUnix/EmApplicationFltk.cpp`) as the reference implementation:

**File**: `src/platform/EmApplicationQt.h/cpp`

```
class EmApplicationQt : public EmApplication {
    Bool Startup(int argc, char** argv) override;
    void Shutdown() override;
    void HandleIdle() override;
    // Clipboard support
    // Window creation
};
```

**Startup sequence** (must match FLTK):
1. Call `EmApplication::Startup(argc, argv)` -- this calls `gPrefs->Load()`, `CSocket::Startup()`, `Debug::Startup()`, `RPC::Startup()`, `LogStartup()`, and parses command-line args
2. Create the window (`EmWindow::NewWindow()` → `EmWindowQt`)
3. Initialize menus (`MenuInitialize()`)
4. Set up clipboard polling

**Run loop**: Replace FLTK's `while(1) { Fl::wait(0.1); HandleIdle(); }` with Qt's event loop + QTimer:
```
// In main():
EmApplicationQt app;
app.Startup(argc, argv);
app.HandleStartupActions();
// Start idle timer
QTimer idleTimer;
connect(&idleTimer, &QTimer::timeout, [&]() {
    if (app.GetTimeToQuit()) { QApplication::quit(); return; }
    app.HandleIdle();
});
idleTimer.start(100); // ~10 Hz, matching FLTK's Fl::wait(0.1) polling interval
QApplication::exec();
app.Shutdown();
```

**Shutdown sequence**: Delete window first (saves position to prefs), then call `EmApplication::Shutdown()`.

### 2.2 `main()` entry point

**File**: `src/ui/main.cpp`

```cpp
int main(int argc, char** argv) {
    QApplication qtApp(argc, argv);
    EmulatorPreferences prefs;
    EmApplicationQt app;

    if (app.Startup(argc, argv)) {
        app.HandleStartupActions();
        // idle timer setup here
        qtApp.exec();
    }
    app.Shutdown();
    return gErrorHappened ? 2 : gWarningHappened ? 1 : 0;
}
```

Key: `EmulatorPreferences` and `EmApplicationQt` are stack objects with proper lifetime, exactly as in FLTK's `main()`.

### 2.3 Reference files
- `Emulator_Src_3.5/SrcUnix/EmApplicationFltk.cpp` (the model to follow)
- `Emulator_Src_3.5/SrcShared/EmApplication.cpp` (base class)

---

## Phase 3: Threading Model

### 3.1 Two-thread architecture (match original POSE)

**UI Thread** (Qt main thread):
- Runs `QApplication::exec()`
- QTimer fires `HandleIdle()` at ~100Hz
- `HandleIdle()` → clipboard sync → `HandleDialogs()` → `EmApplication::HandleIdle()` → document idle → window idle (screen paint)
- Handles all Qt events (mouse, keyboard, menu)
- Calls `EmSessionStopper` when needed for commands (Save, Reset, etc.)

**CPU Thread** (omni_thread, created by `EmSession::CreateThread()`):
- Runs m68k emulation loop
- Communicates with UI via thread-safe queues (`EmPenQueue`, `EmKeyQueue`, `EmButtonQueue`)
- Suspends/resumes via `EmSuspendState` counters
- Blocks on `BlockOnDialog` when error dialogs needed

**No bridge thread.** The v1 bridge thread was a workaround for not understanding that FLTK's idle handler runs on the UI thread. Qt's QTimer provides the same mechanism.

### 3.2 Fix `omni_thread::self()`

The omnithread library uses `pthread_getspecific()` for thread identity. The CPU thread is created via `omni_thread::start()` which sets up the thread-local data. The problem in v1 was likely that `self()` returned nullptr because the thread was created outside omnithread's control.

**Fix**: Ensure the CPU thread is created exclusively through `EmSession::CreateThread()` → `omni_thread::start()`. Never create threads via `QThread` or raw pthreads for emulator-facing work.

If `omni_thread::self()` still fails (e.g., the TLS key setup is broken), add a `thread_local omni_thread*` fallback:
```cpp
// In omnithread.h or a wrapper:
thread_local omni_thread* tl_self = nullptr;
// Set in omni_thread::wrapper() before calling run_undetached()
```

### 3.3 `HandleIdle()` screen painting

In FLTK, `HandleIdle()` calls `EmWindow::HandleIdle()` which calls `HostPaintLCD()` and friends. These run on the UI thread and can directly update the widget.

In Qt, same model: QTimer fires on UI thread, calls `HandleIdle()`, which calls `HostPaintLCD()`. Since we're already on the UI thread, we can directly call `update()` on the QWidget. No cross-thread signaling needed for screen updates.

### 3.4 `BlockOnDialog` handling

When the CPU thread hits an error, it calls `BlockOnDialog()`, which:
1. Sets state to `kBlockedOnUI`
2. Signals the condition variable
3. Waits for UI thread to dismiss the dialog

The UI thread detects `kBlockedOnUI` during `HandleDialogs()` (called from `HandleIdle()`), shows the dialog, and calls `UnblockDialog()` when dismissed.

This works naturally with QTimer driving `HandleIdle()` -- no changes needed from the FLTK model.

---

## Phase 4: Window and Input -- EmWindowQt

### 4.1 Create `EmWindowQt`

**File**: `src/platform/EmWindowQt.h/cpp`

```
class EmWindowQt : public EmWindow, public QWidget {
    // QWidget overrides
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void closeEvent(QCloseEvent*) override;

    // EmWindow overrides (called from UI thread via HandleIdle)
    void HostWindowReset() override;
    void HostPaintLCD(...) override;
    void HostPaintCase(...) override;
    void HostGetCurrentMouse(int* x, int* y) override;
    void HostRectFrame(...) override;
    void HostOvalPaint(...) override;
};
```

### 4.2 Keyboard input (missing from v1)

Map Qt key events to Palm key events via `gSession->PostKeyEvent()`. Reference: FLTK version's keyboard handling in `EmWindowFltk.cpp`.

### 4.3 Screen rendering

`HostPaintLCD()` receives raw framebuffer data. Convert to QImage (handle 1-bit, 2-bit, 4-bit, 8-bit, 16-bit formats). Store as member; `paintEvent()` draws skin background + LCD image.

The v1 code had good pixel format conversion code in `EmWindowQt.cpp` that can be reused.

### 4.4 Reference files
- `Emulator_Src_3.5/SrcUnix/EmWindowFltk.h/cpp` (FLTK reference)
- `abandoned/qt-pose/src/platform/EmWindowQt.cpp` (v1 pixel conversion code to salvage)

---

## Phase 5: Dialog System

### 5.1 Priority dialogs

Implement in order of necessity:

1. **New Session dialog** (`EmDlg::DoSessionNew`) -- v1 had this working, reuse
2. **Common dialog / error reporting** (`EmDlg::DoCommonDialog`) -- needed for error display
3. **File open/save dialogs** (`HostDialogOpen`, `HostRunSessionSave`) -- use QFileDialog
4. **Database import** (`EmDlg::DoDatabaseImport`) -- for PRC installation

### 5.2 Remaining dialogs (lower priority)

Preferences, Logging, Debugging, Error Handling, Skins, HostFS, Breakpoints, About Box -- implement as needed, stub others to return sensible defaults.

### 5.3 Reference files
- `Emulator_Src_3.5/SrcUnix/EmDlgFltk.cpp` (FLTK dialog implementations)
- `abandoned/qt-pose/src/ui/poseapplication.cpp` (v1 New Session dialog to reuse)

---

## Phase 6: Platform Stubs

### 6.1 Required platform files

| File | Purpose | Complexity |
|------|---------|------------|
| `Platform_Unix.cpp` | Clipboard globals, platform queries | Low -- mostly exists |
| `EmFileRefUnix.cpp` | File path operations | Low -- standard Unix |
| `EmTransportSerialUnix.cpp` | Serial port (stub OK) | Low -- return errors |
| `EmDlgQt.cpp` | Dialog dispatch | Medium -- Phase 5 |

### 6.2 Build system

**File**: `src/CMakeLists.txt`

Key differences from v1:
- **No `-Wno-int-to-pointer-cast`** -- these warnings are now errors
- **No `-fpermissive`** -- fix the code instead
- **C++17** (not C++20 -- minimize language requirement)
- **Qt6::Core, Qt6::Widgets** -- same as v1
- **pthread, z, X11** -- same as v1
- Define `PLATFORM_UNIX`, `EMULATION_LEVEL=EMULATION_UNIX`, `_REENTRANT`

---

## Phase 7: ReControl Integration Plan

**Not implemented initially**, but designed for safe future integration.

### 7.1 Lessons from RePOSE4

The three critical bugs were all caused by **external thread directly manipulating EmSession state**:
1. Suspend counter orphaning when exceptions occur during `ExecuteSubroutine`
2. `BlockOnDialog` creating unreachable states
3. Race conditions between UI thread and socket thread both calling `EmSessionStopper`

### 7.2 Safe architecture: Command queue model

Instead of the socket thread directly calling `EmSessionStopper` and `ExecuteSubroutine`, use a **command queue processed on the UI thread**:

```
Socket Thread                    UI Thread (QTimer idle)
    |                                |
    |--- push command to queue ----->|
    |                                | HandleIdle():
    |                                |   process ReControl queue
    |                                |   call EmSessionStopper (only from UI thread)
    |                                |   execute command
    |                                |   push result to response queue
    |<-- read response from queue ---|
```

**Key invariant**: Only the UI thread ever calls `EmSessionStopper` or manipulates session state. The socket thread is purely I/O.

This eliminates:
- Race conditions between threads calling `EmSessionStopper` simultaneously
- The need for `ForceResume()` nuclear option
- Unrecoverable states from competing thread access

### 7.3 Reuse POSE's existing debugging infrastructure

Rather than reimplementing debugging from scratch (as ReControl partially did), route commands through existing POSE mechanisms:

| ReControl Command | POSE Mechanism |
|---|---|
| `break <addr>` | `EmSession::AddInstructionBreakHandlers()` + `MetaMemory::MarkInstructionBreak()` |
| `peek`/`poke` | `EmMemGet32/Put32` with `CEnableFullAccess` |
| `regs` | `EmCPU68K` register access |
| `install` | `EmFileImport` (same as drag-and-drop) |
| `screenshot` | `EmScreen::GetBits()` |
| `state` | `EmSession::GetSessionState()` |
| `reset` | `EmSession::ScheduleReset()` |
| `step`/`continue` | `EmSession` suspend/resume with `fSuspendByDebugger` |
| `trace` | Existing `HostControl` trap tracing |

### 7.4 BlockOnDialog recovery

When the CPU thread is in `kBlockedOnUI` state, the socket API should be able to:
1. Detect this state via `GetSessionState()`
2. Dismiss the dialog programmatically by calling `UnblockDialog()` from the UI thread
3. This is safe because the command queue processes on the UI thread, which is the same thread that would normally dismiss the dialog

### 7.5 Implementation timeline

ReControl should be added **after** the emulator is fully stable and tested without it. Estimated order:
1. Socket listener thread with command parsing (text protocol, same as RePOSE4)
2. Read-only commands first: `state`, `regs`, `peek`, `screenshot`, `ui`
3. Safe commands: `key`, `tap`, `pen`, `button` (use existing thread-safe queues)
4. State-modifying commands: `install`, `launch`, `reset` (via UI thread command queue)
5. Debug commands: `break`, `step`, `continue` (via UI thread command queue)

---

## Build and Verification Plan

### Stage 1: Compiles and links
- Fix type system (Phase 1.3-1.4)
- Stub platform layer
- Target: `cmake --build` succeeds with zero pointer-cast warnings

### Stage 2: Boots to session dialog
- EmApplicationQt lifecycle (Phase 2)
- New Session dialog
- Target: launches, shows dialog, user can select ROM and device

### Stage 3: Runs PalmOS
- EmWindowQt screen rendering (Phase 4)
- Threading model (Phase 3)
- Target: boots PalmOS ROM, shows launcher, LCD updates

### Stage 4: Full interaction
- Keyboard/pen input
- Context menu with commands
- Session save/load
- Target: can tap apps, type text, save/restore sessions

### Stage 5: ReControl (future)
- Implement per Phase 7 plan
- Test each command category individually
- Stress test with rapid command sequences

### Testing approach
- Test in CloudpilotEmu first for reference behavior comparison
- Use PalmOS m500 ROM (same as ShadowStan development)
- Test session save/load round-trip
- Test soft/hard/debug reset
- Verify no pointer-cast warnings at any point

---

## Phase 5 Completion: LP64 sizeof/offsetof Fixes (DONE)

Completed 2026-02-15. Commit `fcf1ccf`.

### Problem

After Phases 1-4, the emulator compiled and linked on 64-bit Linux, but PalmOS
could not boot from a fresh ROM load or survive a soft reset. Two classes of
LP64 bugs remained:

1. **sizeof(host-pointer) in emulated-memory arithmetic**: `sizeof(MemPtr)`,
   `sizeof(void*)`, `sizeof(WinHandle)`, and `sizeof(SysLibTblEntryType)` all
   return 8 on LP64, but PalmOS pointers and structs use 4-byte pointers. This
   corrupted heap walks (MPT size doubled), dispatch table boundaries (MetaMemory
   mismarked heap as system globals), library dispatch lookups (wrong stride into
   library table), and a menu bug workaround (dead comparison).

2. **offsetof(PalmOSStruct, field) with pointer fields before target**: When a
   PalmOS struct contains pointer fields (`MemPtr`, `void*`, `MemHandle`, etc.)
   before the field being accessed, `offsetof()` on the host gives a larger
   offset than the m68k layout. This shifted reads into the wrong fields for
   10+ struct accesses across EmLowMem, Logging, Miscellaneous, EmPatchState,
   and EmPatchModuleSys.

3. **Unmapped memory access**: `EmBankRegs` unconditionally called
   `InvalidAccess()` (fatal bus error) when `GetSubBank()` returned NULL for
   addresses with no registered hardware handler (e.g., 0xFFFFFFFF in DragonBall
   VZ register space). Real hardware silently returns 0xFF for unmapped register
   reads. `EmBankDummy` had the same issue, plus an off-by-one that left bank
   0xFFFF uninitialized.

### Fixes Applied (12 files)

**sizeof fixes:**

| File | Line | Before | After |
|------|------|--------|-------|
| `EmPalmHeap.cpp` | 1743 | `sizeof(MemPtr)` | `sizeof(emuptr)` |
| `MetaMemory.cpp` | 618 | `sizeof(void*)` | `sizeof(emuptr)` |
| `MetaMemory.cpp` | 1225 | `sizeof(WinHandle)` | `sizeof(emuptr)` |
| `EmPalmFunction.cpp` | 1001 | `sizeof(SysLibTblEntryType)` (=32) | `16` |
| `EmPalmFunction.cpp` | 1006 | `sizeof(SysLibTblEntryTypeV10)` (=16) | `8` |
| `Miscellaneous.cpp` | 2127 | `sizeof(SysLibTblEntryType)` (=32) | `16` |
| `Miscellaneous.cpp` | 2132 | `sizeof(SysLibTblEntryTypeV10)` (=16) | `8` |

**offsetof fixes (hardcoded to m68k-palmos-gcc cross-compiled values):**

| File | Struct.Field | m68k | LP64 |
|------|-------------|------|------|
| `EmLowMem.cpp` | `SysEvtMgrGlobalsType.idle` | 37 | 55 |
| `Logging.cpp` | `WindowType.nextWindow` | 36 | 64 |
| `Logging.cpp` | `WindowType.windowFlags` | 8 | 16 |
| `Logging.cpp` | `FormType.formId` | 40 | 72 |
| `Miscellaneous.cpp` | `ControlType.style` | 16 | 26 |
| `EmPatchState.cpp` | `SysAppInfoType.dbP` | 16 | 32 |
| `EmPatchState.cpp` | `SysAppInfoType.stackP` | 20 | 40 |
| `EmPatchState.cpp` | `SysAppInfoType.memOwnerID` | 28 | 48 |
| `EmPatchState.cpp` | `SysAppInfoType.stackEndP` | 44 | 88 |
| `EmPatchModuleSys.cpp` | `SysAppInfoType.cmd` | 0 | 0 |
| `EmPatchModuleSys.cpp` | `SysAppInfoType.dbP` | 16 | 32 |
| `EmPatchModuleSys.cpp` | `DmAccessType.openP` | 6 | 12 |
| `EmPatchModuleSys.cpp` | `DmOpenInfoType.hdrID` | 12 | 24 |
| `EmPatchModuleSys.cpp` | `DmOpenInfoType.cardNo` | 24 | 48 |

**Memory bank fixes:**

| File | Change |
|------|--------|
| `EmBankDummy.cpp` | Bank count `0xFFFF` -> `0x10000` (off-by-one) |
| `EmBankDummy.cpp` | Gate `InvalidAccess` on `fValidate_DummyGet/Set` flags |
| `EmBankDummy.cpp` | Fix Set methods: `forRead=true` -> `forRead=false` |
| `EmBankRegs.cpp` | Gate 6 fallthrough `InvalidAccess` calls on `fValidate_RegisterGet/Set` |
| `EmMemory.cpp` | Default `fValidate_DummyGet`, `RegisterGet`, `RegisterSet` to `false` |

**Guard:**

| File | Change |
|------|--------|
| `EmTypes.h` | `static_assert(sizeof(emuptr) == 4, "emuptr must be 4 bytes")` |

### Verification Method for offsetof Values

All m68k offsets were verified by cross-compiling test structs with
`m68k-palmos-gcc` and reading the offsets from the object file:

```bash
m68k-palmos-gcc -c /tmp/check.c -o /tmp/check.o -I/opt/palmdev/sdk-4/include \
  -I/opt/palmdev/sdk-4/include/Core -I/opt/palmdev/sdk-4/include/Core/System \
  -I/opt/palmdev/sdk-4/include/Core/UI
m68k-palmos-objdump -s -j .data /tmp/check.o
```

### Result

PalmOS boots from a fresh ROM load all the way to the system setup screen.
The cascading heap corruption errors on soft reset are eliminated.

### Storage Heap Error (Resolved)

An initial test showed a "Date & Time (4.0) just wrote to memory location
0x0004076A, which is in the storage heap" error. This turned out to be caused
by the `sizeof(SysLibTblEntryType)` bug (32 on LP64 vs 16 on m68k) which was
already included in the Phase 5 commit. The wrong library table stride caused
POSE to misidentify library functions during boot, disrupting the trap patching
system. With the correct stride, the ROM's csDSelect register writes correctly
toggle `fProtect_SRAMSet` (the hardware SRAM write-protection flag), and the
storage heap error no longer appears.

Key insight: `fProtect_SRAMSet` is not a static flag -- it is dynamically
driven by bit 0x2000 of the DragonBall VZ `csDSelect` chip-select register.
The ROM clears this bit before writing to the storage heap and sets it back
after. The `EmRegsVZ::csDSelectWrite` handler updates `fProtect_SRAMSet`
whenever the register is written.

### Known Remaining Issue

After running for a while, the LCD display may become "squished" (rendered at
wrong dimensions). This appears to be a screen rendering issue in the Qt
painting code, not an LP64 bug. Investigation pending.

---

## Critical Files Reference

| File | Role |
|------|------|
| `Emulator_Src_3.5/SrcShared/EmTypes.h` | Type definitions -- must fix `int32`/`uint32` |
| `Emulator_Src_3.5/SrcShared/EmApplication.h/cpp` | Base app class -- lifecycle management |
| `Emulator_Src_3.5/SrcUnix/EmApplicationFltk.cpp` | Reference platform implementation |
| `Emulator_Src_3.5/SrcShared/EmSession.h/cpp` | Threading, suspend/resume, BlockOnDialog |
| `Emulator_Src_3.5/SrcShared/Hardware/EmMemory.h` | Memory access layer, bank translation |
| `Emulator_Src_3.5/SrcShared/UAE/newcpu.h` | CPU state, `regs.pc_p` vs `regs.pc` |
| `Emulator_Src_3.5/SrcShared/DebugMgr.h/cpp` | Built-in debugging infrastructure |
| `Emulator_Src_3.5/SrcShared/HostControl.h/cpp` | Host trap selectors |
| `Emulator_Src_3.5/SrcShared/Marshal.cpp` | Parameter marshalling (~80 cast sites) |
| `abandoned/qt-pose/src/platform/EmWindowQt.cpp` | v1 pixel conversion code to salvage |
| `abandoned/qt-pose/src/ui/poseapplication.cpp` | v1 New Session dialog to reuse |
