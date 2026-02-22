# ReControl TCP Protocol Reference

POSE64's TCP control interface for programmatic emulator control.

## Connection

- **Transport:** TCP, localhost only
- **Protocol:** Text-based, one command per line, `\n` terminated
- **Connections:** One active session at a time (additional connections get `ERR busy`)
- **Port conventions:** 6416 (dev), 6425 (secondary), 6427 (automated testing)
- **Command line:** `./build/pose64 -psf <rom.psf> --port <port>`

## Response Format

All responses start with `OK` or `ERR`:

- **Single-line:** `OK [data]\n` or `ERR <category>: <message>\n`
- **Multi-line:** First line `OK [header]\n`, data lines indented with space,
  terminated by a line containing only `.\n`

Error categories: `usage` (bad syntax), `transient` (temporary state issue),
`timeout` (CPU didn't reach safe point), `fatal` (recommend reset).

## Commands

### State

| Command | Response | Description |
|---------|----------|-------------|
| `state` | `OK running\n` | Session state: running, suspended:reason, stopped, blocked_on_ui |
| `info` | Multi-line | Device info: version, device, RAM, ROM, screen size, session path |
| `apps` | Multi-line | Installed applications: name, type (4CC), creator (4CC) |

### Input

| Command | Response | Description |
|---------|----------|-------------|
| `tap <x> <y>` | `OK\n` | Pen down + up at display coordinates (0-159) |
| `tap-id <id>` | `OK <x> <y>\n` | Tap center of form object by Palm OS object ID |
| `pen <down\|up> <x> <y>` | `OK\n` | Separate pen down or up event |
| `key <charcode>` | `OK\n` | Single key event (decimal character code) |
| `type <text>` | `OK\n` | Bulk text entry (UTF-8 in, converted to Latin-1) |
| `button <name> <action>` | `OK\n` | Hardware button: power/up/down/app1-4/cradle/contrast, down/up/tap |

### Screen

| Command | Response | Description |
|---------|----------|-------------|
| `screenshot <path>` | `OK <crc32> <w> <h>\n` | Save PNG to path, returns CRC32 hash + dimensions |
| `screen-hash` | `OK <crc32> <w> <h>\n` | CRC32 of screen pixels without saving (fast change detection) |
| `ui` | Multi-line | Active form structure: object types, IDs, labels, bounds, text content |

### Session

| Command | Response | Description |
|---------|----------|-------------|
| `install <path>` | `OK\n` | Install .prc/.pdb file into emulated Palm OS (max 4MB) |
| `launch <dbname>` | `OK\n` | Launch app by database name (supports names with spaces) |
| `save <path>` | `OK\n` | Save session state to .psf file |
| `load <path>` | `OK\n` | Load session from .psf file (destroys current session) |
| `reset [soft\|hard\|debug]` | `OK\n` | Reset emulator (default: soft) |
| `sleep <ms>` | `OK\n` | Pause command processing for 1-30000 ms |
| `quit` | `OK\n` | Exit emulator |

## Multi-line Response Examples

### `ui` response

```
OK FORM id=1000 "Date Book"
  TITLE "Feb 22, 26" (0,0,0,0)
  BUTTON id=1004 "All" (134,0,26,13)
  BUTTON id=1015 "New" (44,147,22,12)
 *FIELD id=1109 (0,16,153,121) "Meeting notes here..."
  LIST id=1005 (86,1,72,0) sel=0 top=0
   [0] "Item 1"
   [1] "Item 2"
  SCROLLBAR id=1008 (153,18,7,140) val=0 min=0 max=10
.
```

Object types: TITLE, BUTTON, FIELD, LIST, LABEL, SCROLLBAR, GADGET, POPUP,
TABLE, GRAFFITI.  The `*` prefix marks the focused object.  Bounds are
`(x, y, width, height)` in form-local coordinates.

### `apps` response

```
OK
 Memo Pad type=appl creator=memo
 Date Book type=appl creator=date
 Address Book type=appl creator=addr
.
```

### `info` response

```
OK POSE64 0.9.0
 device=PalmM515
 ram=16MB
 rom=Palm-m515-4.1-en.rom
 screen=160x160
 session=/path/to/session.psf
.
```

## Coordinate Systems

- **Display coordinates:** (0,0) at top-left of the 160x160 display area.
  Used by `tap`, `pen`, and reported by `tap-id`.
- **Form-local coordinates:** Object bounds in `ui` output are relative to the
  form's window origin.  For full-screen forms, this equals display coordinates.
  For popup dialogs, the form window has a non-zero origin — `tap-id` handles
  this automatically.
- **Object IDs:** Stable integers assigned by the Palm OS form resource.  Use
  `tap-id` instead of coordinates for reliable interaction.

## Screen Change Detection

The `screen-hash` command returns a CRC32 hash computed over the RGB pixel data
of the current frame.  Use it to detect screen changes without capturing a full
PNG:

1. Record hash after each action
2. If hash matches previous, screen hasn't changed — skip the screenshot
3. `screenshot` also returns the hash, so a single capture gives both the
   image and the hash

## Text Encoding

Palm OS uses Latin-1 (ISO 8859-1) internally.  The `type` command accepts UTF-8
input and converts to Latin-1.  The `ui` command returns Latin-1 text.  Use
`latin-1` decoding when reading multi-line responses in Python.

## Tools

### CLI helper (`scripts/rc.py`)

One-shot command execution from the shell:

```bash
python3 scripts/rc.py --port 6416 state
python3 scripts/rc.py --port 6416 ui
python3 scripts/rc.py --port 6416 tap-id 1005
python3 scripts/rc.py --port 6416 type Hello World
python3 scripts/rc.py --port 6416 apps
```

Exit code 0 on OK, 1 on ERR.

### Python client (`test_recontrol.py`)

```python
from test_recontrol import ReControlClient
c = ReControlClient(port=6416, timeout=5)
c.connect()
resp = c.send_command("state")       # single-line
resp = c.read_multiline_response()   # after sending "ui\n" manually
c.disconnect()
```

### Stress test (`test_recontrol_stress.py`)

```bash
# Self-launching (starts its own pose64 instance)
python3 test_recontrol_stress.py --psf m515.psf --port 6427

# Against existing instance
python3 test_recontrol_stress.py --no-launch --port 6416
```

## Implementation

- **File:** `src/core/ReControl.cpp` (~980 lines)
- **Architecture:** `ReControlServer` (QTcpServer) creates `ReControlSession`
  (QObject per connection).  All I/O on the Qt main thread.  CPU-dependent
  commands dispatch to `CPUWorkerThread` via `QueueWork`/`QueueWorkResult`.
- **Thread safety:** QPointer guards all response lambdas against
  session-destroyed races.  See `docs/stability-findings.md` for full analysis.
