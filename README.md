# QtPortPOSE — Palm OS Emulator on Qt6/64-bit Linux

A native 64-bit port of POSE (Palm OS Emulator) 3.5 from FLTK to Qt6,
running on modern x86_64 Linux without `-m32`.

## Status

The emulator boots PalmOS, renders device skins, runs 3rd-party apps (SimCity,
etc.), handles Gremlins automated testing, and is stable for extended sessions.

### What works

- Full application lifecycle (`EmApplicationQt` subclass with proper
  `Startup()` / `Shutdown()` / `HandleIdle()`)
- Session loading from `.psf` files (all subsystems deserialize correctly)
- CPU thread via omnithread (two-thread model matching original POSE)
- LCD rendering (all pixel formats: 1-bit through 16-bit)
- Pen/mouse input
- Keyboard input (F1-F4 hardware buttons, arrow keys, Page Up/Down, printable characters)
- Context menu with full POSE menu system (right-click or F10)
- Skin rendering (JPEG skins from `.skin` files at 1x and 2x scales)
- Button press visual feedback (highlight rectangle on hardware button clicks)
- LED indicator (green power LED)
- New Session dialog, session save prompt, file dialogs
- Database import (PRC/PDB installation via menu)
- Reset dialog (soft/hard/debug reset)
- Common error/warning/info dialogs
- Clipboard sync (bidirectional, text)
- QTimer-based idle loop at ~10 Hz (replaces FLTK's `Fl::wait(0.1)`)
- Gremlins automated testing (10,000+ events across multiple apps)

### What doesn't work yet

- Some menu commands (preferences, logging, debugging dialogs are stubs)
- Database export
- ReControl socket API (planned for Phase 7)

## Quick Start

```bash
# Build (release)
cd build/
cmake ../src
make -j$(nproc)

# Run
./qtpose

# Run with a saved session
./qtpose -psf /path/to/SomeProfile.psf
```

Output binary: `build/qtpose`

### Building the original 32-bit FLTK version

The original POSE 3.5 FLTK build is still available:

```bash
./build.sh
```

Output binary: `src/Emulator_Src_3.5/BuildUnix/pose`

## Debugging

The build system has a `POSE_DEBUG` CMake option that enables ASAN, UBSAN,
debug symbols, and POSE's dormant diagnostic history buffers — without
touching the release build.

```bash
# Debug build (ASAN + UBSAN + debug symbols)
cmake ../src -DPOSE_DEBUG=ON
make -j$(nproc)

# Run with leak detection
ASAN_OPTIONS=detect_leaks=1 ./qtpose

# Run under GDB (for CPU spike diagnosis)
gdb --args ./qtpose
(gdb) handle SIGPIPE nostop noprint pass
(gdb) run
# Ctrl+C during a spike, then: thread apply all bt

# Profile with perf (no recompile needed)
perf record -p $(pidof qtpose) -g --call-graph dwarf sleep 5
perf report

# Switch back to release
cmake ../src -DPOSE_DEBUG=OFF
make -j$(nproc)
```

The `POSE_DEBUG` build adds:
- `-g3 -O1 -fno-omit-frame-pointer` for complete GDB stack traces
- `-fsanitize=address,undefined` for memory and UB error detection
- `-D_DEBUG` which activates 512-entry register/exception history buffers
- An optional `-DPOSE_ASSERTIONS=ON` flag to enable all 1,194 `EmAssert()` calls

See `docs/debugging-guide.md` for the full reference (sanitizer options, GDB
workflows, history buffer inspection, strace/Valgrind usage, and the built-in
logging system).

## Prerequisites

### Qt6 port (64-bit)

| Package (Arch/Manjaro) | What For |
|---|---|
| `base-devel` | gcc, g++, make, cmake |
| `qt6-base` | Qt6 Core + Widgets |
| `libx11` | X11 (for clipboard/display) |
| `zlib` | Compression (session files) |

### Original FLTK build (32-bit)

| Package (Arch/Manjaro) | What For |
|---|---|
| `base-devel` | gcc, g++, make, autoconf, automake |
| `lib32-glibc`, `lib32-gcc-libs` | 32-bit C runtime |
| `lib32-libx11`, `lib32-libxext` | 32-bit X11 |
| `perl` | Generates `ResStrings.cpp` from `Strings.txt` |

## Directory Layout

```
QtPortPOSE/
├── README.md
├── build.sh                    # Original 32-bit FLTK build script
├── src/
│   ├── CMakeLists.txt          # Qt6 port build (C++17, native 64-bit)
│   ├── core/                   # POSE 3.5 engine (modified for LP64)
│   │   ├── UAE/                #   m68k CPU emulator
│   │   ├── Hardware/           #   Device emulation (EmRegs*)
│   │   ├── Patches/            #   ROM patches
│   │   ├── Palm/               #   SDK headers
│   │   ├── omnithread/         #   POSIX threading primitives
│   │   ├── jpeg/               #   JPEG library
│   │   └── Gzip/               #   Gzip (excluded; uses system zlib)
│   ├── platform/               # Qt6 platform implementations
│   │   ├── EmApplicationQt.*   #   App lifecycle (Startup/Shutdown/Run)
│   │   ├── EmWindowQt.*        #   Window, LCD rendering, input
│   │   ├── EmDlgQt.*           #   Dialog dispatch
│   │   ├── Platform_Unix.cpp   #   Host platform queries
│   │   └── Em*Unix.cpp         #   File/dir/transport stubs
│   ├── ui/
│   │   └── main.cpp            #   Entry point
│   ├── Emulator_Src_3.5/       # Unmodified POSE source (FLTK build)
│   ├── fltk-1.1.10/            # FLTK source (FLTK build)
│   └── fltk-install/           # FLTK install prefix (FLTK build)
├── abandoned/                  # Failed v1 Qt port (reference only)
├── docs/                       # Architecture docs and post-mortems
├── sources/                    # Original source tarballs
├── patches/                    # Patches for FLTK build
├── generated/                  # Pre-generated sources for FLTK build
└── Skins/                      # Emulator UI skins
```

## Architecture

### v1 (abandoned) vs v2

The first Qt port attempt (`abandoned/qt-pose/`) failed due to four
fundamental problems:

1. **Bypassed POSE lifecycle** — `EmApplication::Startup()` was never
   called, so sockets, debugging, RPC, and logging were silently disabled.

2. **Broken 64-bit types** — `uint32` was `typedef unsigned long` (8 bytes
   on LP64). Combined with suppressed cast warnings, this caused silent
   data corruption throughout the codebase.

3. **Broken threading** — A 3-thread model (UI + bridge + CPU) that broke
   `omni_thread::self()` identity checks and introduced deadlocks.

4. **Half-finished implementation** — 25+ dialog stubs, zero keyboard
   input, no socket API.

### v2 design

v2 fixes all four problems:

- **Type system**: `int32`/`uint32` are now `int32_t`/`uint32_t` via
  `<cstdint>`, making `emuptr` genuinely 32-bit. Pointer-int cast warnings
  are compile errors (`-Werror=int-to-pointer-cast`). ~200 cast sites were
  fixed individually.

- **Application lifecycle**: `EmApplicationQt` subclass follows the FLTK
  reference exactly — `Startup()` initializes all subsystems, `HandleIdle()`
  runs from a QTimer, `Shutdown()` tears down cleanly.

- **Two-thread model**: UI thread (Qt event loop + QTimer idle) and CPU
  thread (omnithread). No bridge thread. Matches original POSE exactly.

- **No `-fpermissive`**: The build uses `-Werror=int-to-pointer-cast` and
  C++17. Code is fixed, not suppressed.

### LP64 porting details

The core challenge was that POSE assumed ILP32 (`sizeof(long) == 4`).
On LP64 Linux, `long` is 8 bytes. This affected:

- **EmTypes.h**: The fundamental `int32`/`uint32` typedefs — fixed to
  use `<cstdint>` fixed-width types
- **EmStream serialization**: Every `long` class member read 8 bytes
  from streams containing 4-byte values, corrupting all subsequent reads.
  Fixed in ~15 files including ChunkFile, SessionFile, EmPalmHeap,
  EmEventPlayback, GremlinInfo, HordeInfo, and all version constants.
- **EmMemory**: `sizeof(long)` used as stride in `EmMem_memset` loop
- **HostControl**: `sizeof(long)` and `*(long*)` in `PrvPushLong`
- **Platform_NetLib_Sck**: `long param` for ioctl, `*(long*)` buffer reads
- **Marshal.cpp**: ~80 `emuptr` ↔ host pointer cast sites

See `docs/best32to64portingpractices.md` for the full porting guide.

## Compiler Flags

### Qt6 port

```
CXX standard: C++17
C standard:   C11
Warnings:     -Wall -Werror=int-to-pointer-cast (C++ and C)
              -Werror=pointer-to-int-cast (C only)
No:           -fpermissive, -Wno-int-to-pointer-cast
```

### Original FLTK build

```
CC  = gcc -m32 -std=gnu89
CXX = g++ -m32 -std=gnu++98 -fpermissive
CXXFLAGS += -Wno-narrowing
LDFLAGS  = -m32 -L/usr/lib32
```

## Origin

- POSE 3.5 source: extracted from `pose-3.5-2.src.rpm` (Red Hat, circa 2002)
- FLTK 1.1.10 source: fltk.org release (replaces 1.0.11 from src.rpm)
- Original RPM patches (`*.diff`): from the spec file in the src.rpm
- GCC 15 portability patches: developed February 2026
- Qt6 port: developed February 2026

## License

POSE is GPL v2. FLTK 1.1.10 is LGPL v2 with FLTK exception.
