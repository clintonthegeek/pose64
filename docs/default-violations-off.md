# Justification: Defaulting Access Violation Reports to OFF

## Decision

All `Report*Access` preferences in `PreferenceMgr.h` are changed from
`(true)` to `(false)`.

## Context

POSE 3.5 includes an access checking subsystem that monitors emulated memory
accesses and reports violations to the user via modal dialogs. These checks
were designed as a **developer debugging tool** — they help Palm app developers
find bugs where their code directly accesses OS globals, screen memory, low
memory, freed heap chunks, etc.

The original 32-bit FLTK POSE shipped with all these checks defaulted to ON.

## Why This Change is Necessary

### 1. The defaults were never truly "on" in practice

The 32-bit FLTK POSE's event loop timing coincidentally prevented most
violations from ever manifesting (see `docs/meta-check-accessok-bug.md`).
In instrumented testing:

- 19 of 20 interrupt vector reads occurred inside `CEnableFullAccess` windows
- `HwrSleep` head/tailpatches temporarily unlocked vectors at exactly the
  right moments
- `META_CHECK` branches 2 and 3 silently absorbed ROM code accesses

**The effective behavior of 32-bit POSE was: violations almost never appeared.**
The `(true)` defaults were a fiction — the checks were enabled in configuration
but disabled by timing accident.

### 2. The Qt port breaks the timing coincidence

The Qt6 port changed the event loop, threading model, and emulation yield
points. This altered the relative timing of:

- When `MemKernelInit` marks low memory bits
- When `HwrSleep` patches unlock/relock vectors
- When hardware interrupts fire and `ProcessException` reads vectors

Result: 158,000+ false violation dialogs per boot session from ROM code,
system components, and legitimate OS operations.

### 3. The fixes we applied are necessary but insufficient

We fixed the most egregious false positives:

| Fix | What it prevents |
|-----|------------------|
| `AccessOK()` early exit in `META_CHECK` | POSE C++ code (ProcessException) violations |
| ROM code trusted (branches 2/3 removed) | ROM code violations |
| `InRAMOSComponent` trusted | RAM OS component violations |
| `EmBankSRAM` AccessOK + IsPCInRAM | Storage heap violations from ROM |
| `PrvCheckBelowStackPointerAccess` IsPCInRAM | Stack violations from ROM |

These fixes eliminated violations from ROM and POSE internals. But violations
from RAM-based code STILL fire when:

- `InRAMOSComponent` fails to recognize a system component (hardcoded
  creator list from 2001, database directory not ready during boot)
- A legitimate Palm app reads system globals for performance (SimCity,
  BigClock, many others)

These remaining violations produce **modal dialogs that block emulation**.
When they fire every frame (SimCity) or during boot (UIAppShell), the emulator
becomes unusable — the user cannot dismiss one dialog before the next appears.

### 4. Real Palm devices don't enforce these checks

The access violation system is a POSE-specific debugging feature. Real Palm
hardware has no equivalent — applications run without access restrictions.
Every Palm app that shipped and worked on real hardware will work correctly
with violations disabled.

### 5. The checks remain available for developers

Setting the defaults to `(false)` does NOT remove the checking system. It
only changes the factory defaults. Developers debugging their own Palm apps
can re-enable specific checks through:

- The POSE preferences system (once a Debug Options UI is implemented)
- Editing the preferences file directly
- Future: command-line flags

## What This Does NOT Change

- The `META_CHECK` macro still runs (needed for internal bookkeeping)
- The `ProbableCause` → `GetWhatHappened` pipeline still executes
- The `ScheduleDeferredError` mechanism still works
- All violation detection logic remains intact
- Only the **reporting** (dialog display) is suppressed by default

## Alternative Considered: Fixing InRAMOSComponent

We could try to make `InRAMOSComponent` recognize all system components
correctly. However:

- The creator list (`PrvIsRegisteredPalmCreator`) is hardcoded from 2001
- During early boot, the database directory isn't populated
- Third-party system extensions would still trigger false positives
- Even with perfect detection, legitimate app violations (SimCity) would
  still produce unusable dialog floods

Defaulting to OFF is the only approach that makes the emulator usable while
preserving the debugging capability for opt-in use.

## Files Modified

| File | Change |
|------|--------|
| `src/core/PreferenceMgr.h` | All `Report*Access` defaults: `(true)` → `(false)` |
