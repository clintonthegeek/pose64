<p align="center">
  <img src="docs/banner.png" alt="POSE64 — Palm Operating System Emulator">
</p>

A native 64-bit port of POSE (Palm OS Emulator) 3.5, built with Qt6 for
modern x86_64 Linux.

![Screenshot showing multiple emulated Palm devices](docs/screenshot.jpg)

## Features

- **Full Palm OS emulation** — boots PalmOS 2.0 through 4.1 ROMs with
  DragonBall 328, EZ, and VZ processor emulation
- **Device skins** — skin-shaped frameless windows with per-pixel
  transparency and HiDPI support
- **Gremlins** — automated random-input testing
- **Session management** — save and restore complete device state
- **Clipboard sync** — bidirectional text clipboard with the host
- **Speed control** — 0.25x to 8x realtime, with cycle-accurate timer mode
- **Database management** — install and export PRC/PDB/PQA files

Supported devices include: Palm Pilot, Palm III/IIIc/IIIe/IIIx, Palm V/Vx,
Palm m100/m130/m500/m515, Handspring Visor, and others.

## Prerequisites

| Package (Arch/Manjaro) | What For |
|---|---|
| `base-devel` | gcc, g++, make, cmake |
| `qt6-base` | Qt6 Core + Widgets |
| `libx11` | X11 (clipboard/display) |
| `zlib` | Compression (session files) |

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### Debug build

```bash
cmake .. -DPOSE_DEBUG=ON
make -j$(nproc)
```

See `docs/debugging-guide.md` for the full reference.

## Install

```bash
cmake --install build --prefix /usr/local
```

This installs the binary, skins, desktop entry, and icon.

## Run

```bash
pose64

# With a saved session
pose64 -psf /path/to/Session.psf
```

You will need a Palm OS ROM file to create a new session.

## License

GPL v3 — see [LICENSE](LICENSE) for details.
