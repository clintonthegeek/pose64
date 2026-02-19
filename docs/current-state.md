# POSE64: Current State (Feb 2026)

POSE64 is a native 64-bit Qt6 port of the Palm OS Emulator (POSE) 3.5,
rewritten from scratch over Feb 15-18, 2026.  It runs PalmOS 2.0-4.1 ROMs
on Linux/Wayland with cycle-accurate timer emulation and per-device speed
calibration.

## What Works

### Core Emulation

- **MC68000 CPU** via UAE 0.8.10 opcode handlers, with a `* 2` correction
  for UAE's half-speed cycle accounting
- **DragonBall 328, EZ, and VZ** hardware register emulation — all three
  chip variants have accurate timer paths with Bresenham-style accumulators
- **Accurate timer mode** — timer counters advance by actual CPU cycle cost
  divided by the hardware prescaler, replacing the original hardcoded
  `increment = 4` constant
- **Sleep-until-interrupt** — the STOP loop computes cycles until the next
  timer compare match and sleeps precisely, reducing idle CPU usage from
  100% to near-zero
- **Per-device benchmark calibration** — a lookup table of real-hardware
  vs emulator benchmark results adjusts the throttle clock frequency so
  that "1x Realtime" matches the actual speed of the target device

### Supported Devices

Palm Pilot, Pilot 1000/5000, Palm III/IIIc/IIIe/IIIx/IIIxL, Palm V/Vx,
Palm VIIx, Palm m100/m130, Palm m500/m515/m525, Palm i705, Handspring
Visor/Deluxe, Symbol Workpad, and TRG devices.

Benchmark calibration data currently exists only for the **PalmM500**.
Other devices use uncorrected clock frequencies (still functional, but
1x speed won't precisely match real hardware).

### User Interface

- **Skin-shaped frameless window** with per-pixel alpha transparency,
  optional feathered (anti-aliased) edges, and Wayland-native window dragging
- **Context menu** (right-click or F10) with full emulator control
- **LCD rendering** supporting 1-bit mono through 32-bit color, with
  transparent LCD mode and backlight tint detection
- **Button press feedback** (highlight rectangles) and **LED indicators**
- **HiDPI support** with device-pixel-ratio scaling
- **Stay-on-top** window option

### Speed Control

- Presets: 0.25x, 0.5x, 1x, 2x, 4x, 8x, Max (unthrottled)
- Manual speed with fraction input ("1/32", "3/4")
- Toggle between Accurate Timer and Legacy Timer modes
- Anti-drift throttle: resets baseline after 100ms stalls instead of
  bursting to catch up

### Session & Database Management

- New/Open/Save/Save As sessions (.psf format)
- Save Bound Session (ROM + device state in one file)
- 10-item MRU lists for sessions and databases
- Install PRC/PDB/PQA files
- Export installed databases to host filesystem
- Save Screen (LCD screenshot)
- Soft and cold reset

### Development Tools

- **Gremlins** automated random input testing (new session, step, resume,
  stop, event replay, event minimize)
- **Profiling** (CPU instruction profiling, when compiled with HAS_PROFILING)
- **Breakpoints** manager
- **Logging**, **debugging**, **error handling**, and **tracing** options
- **HostFS** — mount host filesystem directories as Palm VFS volumes

## Known Limitations

### UAE Cycle Accuracy

UAE's opcode handlers return cycle counts that are systematically lower
than real MC68000 timing.  After the `* 2` correction, a NOP costs 4
cycles (correct), but complex instructions like RTS cost 4 instead of 16.
The net effect: the emulator bills ~2.7x more emulated cycles per unit of
real work than the actual hardware uses.

The benchmark calibration corrects this globally for the throttle (so
wall-clock speed is correct), but per-instruction timing is approximate.
Work-bound operations (animations, screen drawing) run at roughly correct
speed.  The timer fires at the correct rate.  But the CPU gets ~2.7x less
work done per timer tick than real hardware.

See `timer-accuracy-findings.md` for the detailed analysis.

### No Per-Region Bus Timing

All memory accesses cost the same emulated cycles regardless of target
address.  Real DragonBall hardware has different timing for Flash ROM,
SDRAM, LCD framebuffer, and hardware registers.  On a real m500, ROM
reads are 27% faster than RAM reads; hardware register reads are 31%
faster.  The emulator treats them identically.

See `benchmark-analysis-m500.md` for the full region analysis.

### Single-Device Calibration Data

Only the Palm m500 has real-hardware benchmark data for the auto-correction
system.  Adding calibration for other devices requires running BenchPRC on
physical hardware (or a cycle-accurate reference emulator) and adding the
results to `EmDeviceBenchmark.h`.

### Gremlins + Accurate Timers

At maximum (unthrottled) speed with accurate timers enabled, PalmOS tick
counters may overflow or produce unexpected behavior.  A user-facing
warning is planned but not yet implemented.

### ReControl Socket API

The original POSE's Unix socket control interface (install PRCs, take
screenshots, inject pen/button events, read memory, launch apps) is not
ported.  Automated testing workflows should use CloudpilotEmu instead.

## Architecture

See `qt-port-architectural-review.md` for the full architectural description,
including the threading model, execution pipeline, timer system internals,
and throttle calibration math.

## Documentation Index

| File | Content |
|------|---------|
| `current-state.md` | This file — feature overview and known limitations |
| `qt-port-architectural-review.md` | Architecture: threading, timers, throttle, window system |
| `timer-accuracy.md` | Problem statement: why the original `increment = 4` was wrong |
| `timer-accuracy-findings.md` | Implementation results: bugs found, diagnostic data, UAE cycle analysis |
| `benchmark-analysis-m500.md` | Real m500 vs emulator benchmark comparison, bus region analysis |
| `winuae-gencpu-fork-postmortem.md` | Post-mortem of attempted WinUAE code generator fork |
| `best32to64portingpractices.md` | General LP64 porting reference |
| `gdb-crash-debugging.md` | GDB debugging techniques for emulator crashes |
