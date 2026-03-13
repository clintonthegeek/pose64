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

Install errors include file path, size, and recovery suggestions.  Example:
```
ERR timeout: CPU did not reach syscall boundary within 6740ms. File: Shadow.prc (174KB). Recovery: palm_reset type=soft, then retry install.
```

## `blocked_on_ui` State

When the CPU encounters an error (illegal instruction, bus error, etc.), it shows
a modal dialog and enters `blocked_on_ui` state.  In this state:

- **`dialog`** — returns dialog message, buttons, AND a full CPU register dump
  (PC, SR, D0-D7, A0-A7) for crash diagnostics.
- **`dialog respond <button>`** — dismisses the dialog; CPU resumes.
- **`reset`** — works unconditionally.  Dismisses any pending dialog, sets the
  reset flag, and unblocks the CPU thread.  Response indicates what happened:
  `OK reset (was blocked_on_ui, dialog dismissed)`.
- **`regs`** — works; registers are frozen and stable.
- **`peek`** — works; memory is stable.
- **`backtrace`** — works; stack crawl from frozen CPU state.
- **`state`** — works; returns `OK blocked_on_ui`.

Commands that require CPU execution (`install`, `launch`, `tap`, etc.) will
fail in this state.  Dismiss the dialog or reset first.

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
| `install <path>` | `OK\n` | Install .prc/.pdb file (max 4MB, timeout scales with file size) |
| `launch <dbname>` | `OK\n` | Launch app by database name (supports names with spaces) |
| `save <path>` | `OK\n` | Save session state to .psf file |
| `load <path>` | `OK\n` | Load session from .psf file (works from cold start or replaces current) |
| `reset [soft\|hard\|debug]` | `OK\n` | Reset emulator (works even in `blocked_on_ui` state) |
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

### Dialog

| Command | Response | Description |
|---------|----------|-------------|
| `dialog` | Multi-line or `OK none\n` | Query pending dialog: message, buttons, CPU registers |
| `dialog respond <button>` | `OK\n` | Dismiss dialog by button name (continue/debug/reset/ok/cancel/yes/no) |

When a dialog is pending and the session is `blocked_on_ui`, the query response
includes a `regs` line with all CPU registers for crash diagnostics:

```
OK
 message=App just executed an illegal instruction at 0x000C.
 button continue Continue
 button debug Debug
 button reset Reset
 regs PC=00012340 SR=2700 D0=00000000 ... A7=000FFFF0
.
```

### Memory & Registers

| Command | Response | Description |
|---------|----------|-------------|
| `peek <addr> <nbytes>` | `OK <hex>\n` | Read 1-256 bytes from emulated memory |
| `poke <addr> <nbytes> <hex>` | `OK\n` | Write 1-256 bytes to emulated memory |
| `regs` | `OK D0=... PC=... SR=...\n` | Dump all m68k registers |
| `menu <"title"> <"item">` | `OK\n` | Trigger menu item by name |
| `delete <dbname>` | `OK\n` | Delete a database |

Address formats: `0x<hex>` (absolute), `a5@<offset>` (A5-relative),
`global.<name>` (named low-memory global).

`regs` and `peek` work in `blocked_on_ui` state (CPU is frozen, state is stable).

### Batch

| Command | Response | Description |
|---------|----------|-------------|
| `run <script>` | `OK\n` | Execute multiple sub-commands separated by `;` |

Sub-commands: `tap`, `pen`, `key`, `type`, `button`, `sleep`, `repeat N { ... }`.

### Debugging

| Command | Response | Description |
|---------|----------|-------------|
| `backtrace` (or `bt`) | Multi-line | Stack crawl with PC and A6 per frame |
| `break list` | Multi-line | List all 6 breakpoint slots with status |
| `break set <idx> <addr> [cond]` | `OK\n` | Set breakpoint at address with optional condition |
| `break clear <idx>` | `OK\n` | Clear breakpoint and condition |
| `break enable <idx>` | `OK\n` | Enable breakpoint |
| `break disable <idx>` | `OK\n` | Disable breakpoint |
| `watch set <addr> <nbytes>` | `OK\n` | Monitor address range for writes |
| `watch clear` | `OK\n` | Remove watchpoint |
| `watch status` | `OK ...\n` | Query watchpoint state |
| `spy set <addr>` | `OK\n` | Monitor single address for value changes |
| `spy clear` | `OK\n` | Remove step spy |
| `spy status` | `OK ...\n` | Query step spy state |

### Logging

| Command | Response | Description |
|---------|----------|-------------|
| `log list` | Multi-line | List 20 logging categories with current values |
| `log set <cat> <0\|1\|2>` | `OK\n` | Set logging level (0=off, 1=gremlin, 2=always) |
| `log dump` | `OK\n` | Flush log buffer to file |
| `log clear` | `OK\n` | Clear log buffer |

### Gremlins

| Command | Response | Description |
|---------|----------|-------------|
| `gremlin new <seed> <events>` | `OK ...\n` | Start automated stress test |
| `gremlin status` | `OK ...\n` | Query gremlin progress |
| `gremlin suspend` | `OK\n` | Pause gremlin |
| `gremlin step` | `OK\n` | Single-step gremlin |
| `gremlin resume` | `OK\n` | Resume gremlin |
| `gremlin stop` | `OK\n` | Stop gremlin |

### Configuration

| Command | Response | Description |
|---------|----------|-------------|
| `check list` | Multi-line | List 18 memory-check flags with on/off status |
| `check set <flag> <on\|off>` | `OK\n` | Toggle individual memory check |
| `check set-all <on\|off>` | `OK\n` | Toggle all memory checks |
| `errorhandling get` | Multi-line | Query error/warning behavior settings |
| `errorhandling set <s> <opt>` | `OK\n` | Set behavior (show/continue/quit/switch) |

### Profiling

| Command | Response | Description |
|---------|----------|-------------|
| `profile init [max] [depth]` | `OK\n` | Initialize profiler |
| `profile start` | `OK\n` | Begin profiling |
| `profile stop` | `OK\n` | Pause profiling |
| `profile dump <path>` | `OK\n` | Write Metrowerks .mwp profile |
| `profile print <path>` | `OK\n` | Write text profile report |
| `profile cleanup` | `OK\n` | Free profiler memory |
| `profile cycles` | `OK ...\n` | Query cycle counters (clock, read, write) |

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

- **File:** `src/core/ReControl.cpp` (~3100 lines)
- **Architecture:** `ReControlServer` (QTcpServer) creates `ReControlSession`
  (QObject per connection).  All I/O on the Qt main thread.  CPU-dependent
  commands dispatch to `CPUWorkerThread` via `QueueWork`/`QueueWorkResult`.
- **Thread safety:** QPointer guards all response lambdas against
  session-destroyed races.  See `docs/stability-findings.md` for full analysis.
