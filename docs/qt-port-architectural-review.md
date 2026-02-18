# QtPortPOSE Architectural Review

> **Status (2026-02-18):** This document was originally written at the start
> of the v2 rewrite to catalogue the v1 Qt port's defects. The v2 rewrite
> (Feb 15-18) addressed every issue listed in "The Bad" and "The Ugly"
> sections. Those sections are preserved below as a record of what was wrong
> and why the rewrite was necessary. The "Current Architecture" section at the
> end describes the system as it stands today.

---

## Original Assessment (v1 Port)

### Architecture Overview: What Changed

The FLTK original has a **two-thread model**: the main thread runs `Fl::wait()` + `HandleIdle()` (event loop AND emulator idle pump on the same thread), while a CPU thread runs m68k instructions. Simple, battle-tested.

The v1 Qt port introduced a **three-thread model**: Qt UI thread (event loop only), a "bridge" thread (runs HandleIdle via `PoseIdleWorker`), and the CPU thread. This was done because Qt's event loop must never block, and `HandleIdle()` can block on `EmSessionStopper`. The concept was sound. The execution is where things went off the rails.

---

### The Good (carried forward into v2)

1. **Bridge thread concept** — Moving `HandleIdle()` off the UI thread was correct. Qt cannot block its event loop.

2. **Cross-thread screen delivery** — `QMetaObject::invokeMethod` with `Qt::QueuedConnection` and QImage copy-by-value. Textbook Qt thread safety.

3. **Atomic mouse position** — `QAtomicInt fMouseX/fMouseY` lets the CPU thread read mouse coordinates without touching QWidget state.

4. **The omnithread shim** — A complete API-compatible replacement of the CORBA omnithread library using QMutex, QWaitCondition, and QThread. 227 lines that saved touching ~18 core files. The best piece of engineering in the v1 port.

5. **emPixMapToQImage** — All 13+ EmPixMap formats correctly converted with deep copies of transient data.

6. **New Session dialog** — Proper Qt dialog with ROM MRU, device filtering, RAM size filtering, browse button, signal/slot wiring.

7. **CMake build system** — Clean Qt6 integration with correctly excluded conflicting sources.

---

### The Bad (all fixed in v2)

1. **`EmApplication::Startup()` was never called.** The bridge cherry-picked two lines from a multi-step init chain. Socket, debug, RPC, logging subsystems were all skipped. *v2 fix: proper `Startup()` call with full lifecycle.*

2. **No platform subclass.** No `EmApplicationQt` existed — clipboard sync and modeless dialog pumping were absent. *v2 fix: the init sequence runs `EmApplication::Startup()` directly; clipboard and dialog stubs are implemented.*

3. **Window ownership inverted.** `main()` created the window independently; nobody stored the pointer. *v2 fix: window creation follows the FLTK lifecycle pattern.*

4. **Zero keyboard input.** No `keyPressEvent()` override — the emulated Palm had no physical buttons and no keyboard input. *v2 fix: keyboard input implemented.*

5. **No ReControl socket API.** The 800+ line socket control interface was missing entirely. *v2 status: ReControl is not ported (not needed — the original use case was automated testing of a specific Palm app which has since moved to CloudpilotEmu for that purpose).*

6. **HostRectFrame/HostOvalPaint were no-ops.** Button press feedback and LED indicators were silently invisible. *v2 fix: both draw proper overlays in `paintEvent()`.*

7. **`EmApplication::Shutdown()` was never called.** Preferences were never saved, resources never cleaned up. *v2 fix: proper shutdown lifecycle.*

---

### The Ugly (all fixed in v2)

1. **64-bit pointer truncation.** `-fpermissive` + `-Wno-int-to-pointer-cast` silently truncated pointers on LP64. *v2 fix: all `emuptr`-to-pointer casts audited and fixed. LP64 elimination log documented every instance. The `-fpermissive` and cast-warning suppressions are gone.*

2. **`omni_thread::self()` returned nullptr.** `InCPUThread()` always returned false, breaking the synchronization model. *v2 fix: `omni_thread::self()` returns the correct thread.*

3. **Three-thread deadlock potential.** Both the bridge thread and UI thread could call `EmSessionStopper` simultaneously. *v2 fix: simplified to two threads (UI + CPU). Menu commands are dispatched correctly.*

4. **`stopBridgeThread` race condition.** Double-quit racing between queued stop and explicit quit. *v2 fix: clean thread shutdown.*

5. **Inside-out initialization sequence.** Init was scattered across four classes with unclear ownership. *v2 fix: linear init sequence following the FLTK pattern.*

6. **25 dialog stubs returning zero/empty.** Session save, preferences, debugging, error handling, Gremlins — all silently failing. *v2 fix: dialogs implemented with Qt equivalents (QDialog, QInputDialog, QMessageBox) or properly stubbed with user-facing fallbacks.*

---

## Current Architecture (v2, Feb 2026)

### Threading Model

Two threads:
- **UI thread** — Qt event loop, window painting, menu handling
- **CPU thread** — `omni_thread` running the m68k execution loop

Communication between threads:
- `fEmulationSpeed` — `std::atomic<int>`, set from UI, read by CPU throttle
- `fEffectiveClockFreq` — `std::atomic<int32>`, lazy-computed on first throttle call
- LCD frames — `QMetaObject::invokeMethod(Qt::QueuedConnection)` posts QImage copies from CPU to UI
- Mouse position — `QAtomicInt fMouseX/fMouseY`

### Execution Pipeline

```
EmCPU68K::Execute()
  ├── fetch opcode
  ├── cycles = functable[opcode](opcode)
  ├── cycles *= 2                          // UAE returns half-speed counts
  ├── fCycleCount += cycles
  ├── EmHAL::Cycle(false, cycles)          // advance hardware timers
  └── every 32768 instructions:
      └── CycleSlowly()                    // throttle + UI sync
```

### Timer System (Accurate Mode)

Each DragonBall variant (328/EZ/VZ) has a Bresenham-style accumulator:

```
Cycle(sleeping, cycles):
    fTmr1CycleAccum += cycles
    ticks = fTmr1CycleAccum >> fTmr1Shift      // prescaler as bit shift
    fTmr1CycleAccum &= fTmr1ShiftMask          // keep fractional remainder
    tmr1Counter += ticks
    if counter >= compare: fire interrupt
```

`fTmr1Shift` is cached on every write to `tmr1Control`, avoiding per-instruction register reads.

`GetCyclesUntilNextInterrupt()` inverts the accumulator to compute how many CPU cycles remain before the next timer compare match — used by the STOP loop to sleep precisely.

### STOP Loop (Sleep-Until-Interrupt)

When the CPU executes `STOP`:

```
ExecuteStoppedLoop():
    cyclesToNext = EmHAL::GetCyclesUntilNextInterrupt()
    clamp to [16, 65536]
    fCycleCount += cyclesToNext
    EmHAL::Cycle(true, cyclesToNext)           // advance timer to near-match
    sleep for wall-clock equivalent using raw system clock
    every 8th iteration: CycleSlowly()         // ~12 Hz for RTC alarm check
```

The STOP loop uses the raw system clock (33 MHz for m500), not the benchmark-corrected clock. Timer hardware ticks at the VZ328's actual clock rate regardless of emulated CPI.

### Speed Throttle with Benchmark Calibration

The throttle in `CycleSlowly()` paces `fCycleCount` against wall-clock time. The clock frequency used is `fEffectiveClockFreq`, computed from per-device benchmark data:

```
effectiveClockFreq = systemClockFreq * emuMixedCPI / realMixedCPI
```

For the m500: `33,161,216 * 3232 / 1214 ≈ 88.3 MHz`.

This corrects for UAE's systematic cycle overcount (the emulator bills ~2.7x more cycles per operation than real hardware uses). The benchmark data lives in `EmDeviceBenchmark.h` as a lookup table keyed by device name.

Anti-drift: if the throttle falls more than 100ms behind (e.g. after a modal dialog stall), it resets its baseline instead of bursting to catch up.

### Window System

- **Frameless skin-shaped window** — `Qt::FramelessWindowHint` + `WA_TranslucentBackground` makes the window match the skin image's shape with per-pixel alpha
- **Feathered edges** — optional Gaussian blur (binomial kernel, radius 2) applied to the alpha mask edge for anti-aliased appearance
- **Transparent LCD** — LCD pixels with alpha 0 show the skin through; backlight detection draws a semi-transparent color wash
- **Compositing order** in `paintEvent()`: skin → optional LCD tint → LCD image → button frame overlay → LED ellipse
- **Window dragging** — left-click on skin border calls `startSystemMove()` for native Wayland/X11 drag
- **Stay-on-top** — optional `Qt::WindowStaysOnTopHint` preference
- **Default skin** — procedural gray rectangle generated if no skin is loaded

### Speed Controls

Right-click context menu → Emulation Speed:
- Timer mode toggle: Accurate Timer / Legacy Timer
- Presets: 0.25x, 0.5x, 1x, 2x, 4x, 8x, Max
- Manual Speed dialog accepting fraction input ("1/32", "3/4", etc.)

### Known Limitations

1. **UAE cycle model is ~2.7x too slow per instruction** relative to real DragonBall hardware. The benchmark calibration corrects this globally for the throttle, but per-instruction timing remains approximate. Work-bound operations (animations, drawing) run at roughly correct wall-clock speed; the per-instruction cycle count is not MC68000-accurate. See `timer-accuracy-findings.md` and `benchmark-analysis-m500.md`.

2. **No per-region bus timing.** All memory accesses cost the same emulated cycles regardless of target address. Real hardware has different timing for Flash, SDRAM, LCD, and hardware registers. See `benchmark-analysis-m500.md` §The Region Problem.

3. **Gremlins + accurate timers.** At maximum speed with accurate timers, PalmOS tick counters may overflow or misbehave. A warning is planned but not yet implemented.
