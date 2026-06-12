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
- **`peek`** / **`poke`** — work; memory is stable.
- **`backtrace`** — works; stack crawl from frozen CPU state.
- **`break`** — works (all sub-commands; GATE 3 Gap 1 fix, 2026-06-12); the
  CPU thread is frozen, so the breakpoint table is safe to modify.  This is
  how you escape a breakpoint on a hot event-loop address: `break clearall`
  while blocked, then `dialog respond continue` — nothing is left to re-hit.
- **`state`** — works; returns `OK blocked_on_ui`.

Commands that require CPU execution (`install`, `launch`, `tap`, etc.) will
fail in this state.  Dismiss the dialog or reset first.

**`load`** refuses in this state with
`ERR blocked: dismiss dialog first with 'dialog respond' (if it re-raises,
'break clearall' works while blocked)` — it must not tear down a session
whose CPU thread is parked on a dialog (GATE 3 Gap 3 fix, 2026-06-12;
pre-fix this deadlocked the server).

Commands that require a CPU cycle boundary (`ui`, `watch`, `spy`,
`peek`/`poke`/`regs`/`backtrace`/`break` when not `blocked_on_ui`) return:
```
ERR timeout: CPU did not reach a cycle boundary within 5000ms. Recovery: dismiss any dialog (dialog respond) or palm_reset.
```

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
| `tap <x> <y>` | `OK delivered\n` | Pen down + up at display coordinates (0-159); blocks ≤2 s until the guest's event queue has it |
| `tap-id <id>` | `OK delivered <x> <y>\n` | Tap center of form object by Palm OS object ID; blocks ≤2 s on delivery |
| `pen <down\|up> <x> <y>` | `OK delivered\n` | Separate pen down or up event |
| `key <charcode>` | `OK delivered\n` | Single key event (decimal character code) |
| `type <text>` | `OK delivered\n` | Bulk text entry (UTF-8 in, converted to Latin-1) |
| `button <name> <action>` | `OK\n` | Hardware button (queued, not delivery-confirmed): power/up/down/app1-4/cradle/contrast, down/up/tap |

`tap`/`tap-id`/`pen`/`key`/`type` are **delivery-honest** (Phase 2): `OK delivered`
means PuppetString handed the event to the Palm OS event queue.  If the guest
cannot take it within 2 s the response is `ERR pending: queued, not delivered
within 2000ms`.  A refused post is reported, never swallowed as `OK`:

| Response | Meaning |
|----------|---------|
| `ERR busy: gremlin running` | Gremlins (Hordes) active — input is suppressed |
| `ERR busy: event playback active` | Event playback in progress |
| `ERR busy: minimization active` | Minimization in progress |
| `ERR duplicate: pen already down at that point` | Identical pen-down to the previous one (invisible to the guest) |
| `ERR pending: queued, not delivered within 2000ms` | Posted, but the guest did not consume it in time |

`button` keeps the queued contract (`OK` = enqueued to the hardware-button
state, not delivery-confirmed) and returns `ERR busy: gremlin or playback
active` when refused.

### Screen

| Command | Response | Description |
|---------|----------|-------------|
| `screenshot [path] [scale=N] [grid] [annotate] [crosshair=X,Y]` | `OK <crc32> <w> <h>\n` | Save PNG (or return base64 if no path); overlay options upscale (max 16), draw coordinate rulers, draw labeled bounding boxes from form data, or mark a point. CRC is always of raw pre-overlay pixels |
| `screen-hash` | `OK <crc32> <w> <h>\n` | CRC32 of screen pixels without saving (fast change detection) |
| `ui` | Multi-line | Active form structure: object types, IDs, labels, bounds, text content |

### Session

| Command | Response | Description |
|---------|----------|-------------|
| `install <path>` | `OK\n` | Install .prc/.pdb file (max 4MB, timeout scales with file size) |
| `launch <dbname>` | `OK\n` | Launch app by database name (supports names with spaces) |
| `save <path>` | `OK\n` | Save session state to .psf file |
| `load <path>` | `OK\n` | Load session from .psf file (works from cold start or replaces current). Refuses while `blocked_on_ui`: `ERR blocked: dismiss dialog first…` |
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
OK POSE64 0.9.1
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

All commands in this section (and Logging, Gremlins, Configuration, Profiling below) are exposed as MCP tools (`palm_break`, `palm_watch`, `palm_spy`, `palm_log`, `palm_gremlin`, `palm_check`, `palm_errorhandling`, `palm_profile`, `palm_backtrace`, `palm_speed`) — raw TCP is no longer needed for any of them.

> **`break` stops execution on hit (Phase 3b, landmine #1 fixed).** When a
> breakpoint fires, `Debug::EnterDebugger` schedules a deferred-error dialog
> (the same Continue/Debug/Reset dialog `watch`/`spy` raise): the CPU blocks on
> it and `state` reports `blocked_on_ui`. Inspect the hit with `dialog` (the
> message names the slot and hit address, and a register dump is appended),
> `backtrace`, `peek`, etc., then resume with `dialog respond continue` (or
> `dialog respond reset`). `break` itself works while `blocked_on_ui` (GATE 3
> Gap 1 fix, 2026-06-12): clear or modify breakpoints from the dialog, then
> continue — for a breakpoint on a hot event-loop address this is the ONLY
> reliable cleanup order (the CPU re-hits before any cycle boundary after a
> bare continue). Note: `watch`/`spy` remain WorkerCycle commands and cannot
> run while `blocked_on_ui`.
>
> If an external Palm-Debugger-protocol (SLP) client is attached instead, that
> debugger takes the hit. As of Phase 3b the SLP listening sockets (ports
> 6414/2000) are **off by default** (landmine #8): launch with `--slp-debugger`
> (or set the `SLPDebugger` preference) to attach an external debugger —
> otherwise nothing listens there and the in-emulator dialog handles the hit.

| Command | Response | Description |
|---------|----------|-------------|
| `backtrace` (or `bt`) | Multi-line | Stack crawl with PC and A6 per frame |
| `break list` | Multi-line | List all 6 breakpoint slots with status |
| `break set <idx> <addr> [cond]` | `OK\n` | Set breakpoint at address with optional condition |
| `break clear <idx>` | `OK\n` | Clear breakpoint and condition |
| `break enable <idx>` | `OK\n` | Enable breakpoint |
| `break disable <idx>` | `OK\n` | Disable breakpoint |
| `break clearall` | `OK\n` | Clear all 6 breakpoint slots at once |
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
| `speed [<percent>\|max]` | `OK\n` / `OK <percent>\n` / `OK max\n` | Set or query emulation speed (100 = 1x wall-clock, `max` = unthrottled). Needed for GATE 2 dual-speed runs. |

> **Check-flag semantics (landmine #7 ROOT-FIXED 2026-06-12 — per-site
> verdict cache):** the historical freeze (pre-fix reproduction: first-2-min
> CPU median 37.5% → last-2-min **99.8%**, RSS **+98.8 MB**/10 min under
> `gremlin 42` with ScreenAccess armed) is gone. Each violating **site** —
> (PC, access kind, size, read/write) — is fully analyzed and reported ONCE
> per arming; repeated hits are counted and suppressed. Distinct violation
> kinds that share a meta-bit signature at one PC coarsen to one report. A
> report can raise a Continue/Debug/Reset dialog (`blocked_on_ui`) — answer
> with `dialog respond continue`. `check clearall` + `check set` re-arms
> fresh analysis (every site re-reports once). Cached verdicts are
> invalidated on reset, session load, any check-flag change, and when the
> code chunk containing a cached PC is unlocked; forgiveness decisions that
> depend on dynamic context (stack walks, live UI objects, transient patch
> state, specific addresses) are never cached. The `gMetaCheckActive`
> short-circuit still makes everything free with flags off. **Measured
> post-fix (10-min acceptance, gremlin 42):** ScreenAccess armed — flagged
> CPU median 40.3% vs baseline 38.4%, RSS growth 0.8 MB, every probe <2 ms;
> all six DRAM flags armed — RSS growth 0.9 MB, probes <2 ms (the guest
> spends that run parked on the first report's dialog, which is the
> once-per-arming contract working).

| `check list` | Multi-line | List 18 memory-check flags with on/off status |
| `check set <flag> <on\|off>` | `OK\n` | Toggle individual memory check |
| `check set-all <on\|off>` | `OK\n` | Toggle all memory checks |
| `check clearall` | `OK\n` | Turn off all 18 MetaMemory check flags |
| `errorhandling get` (or `list`) | Multi-line | Query error/warning behavior settings |
| `errorhandling set <s> <opt>` | `OK\n` | Set behavior (show/continue/quit/switch) |

### Profiling

| Command | Response | Description |
|---------|----------|-------------|
| `profile init [max] [depth]` | `OK\n` | Initialize profiler |
| `profile start` | `OK\n` | Begin profiling |
| `profile stop` | `OK\n` | Pause profiling |
| `profile dump <path>` | `OK\n` | Write Metrowerks .mwp profile + auto-generated .txt sibling |
| `profile print <path>` | `OK\n` | Write text profile report |
| `profile cleanup` | `OK\n` | Free profiler memory |
| `profile cycles` | `OK ...\n` | Query cycle counters (clock, read, write) |

`profile dump` and `profile print` return errors if:
- Profiling is not enabled (must call `profile init` + `profile start` first)
- Profiling is still running (must call `profile stop` first)
- No data was collected (gClockCycles == 0)

`profile dump <path>` writes a Metrowerks `.mwp` file and also auto-generates
a `.txt` sibling with the same base name (e.g., `foo.mwp` produces `foo.txt`).

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

### One-shot commands from the shell

```bash
echo "state" | socat -t5 - TCP:localhost:6416
printf 'ui\n' | socat -t10 - TCP:localhost:6416
printf 'tap-id 1005\n' | socat -t5 - TCP:localhost:6416
```

(There is no bundled CLI helper script: `scripts/rc.py` was deleted on
2026-02-22 when the MCP proxy obsoleted it, but this doc kept referencing it.)

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

- **Files:** `src/core/ReControl.cpp` (TCP server + table-driven dispatch,
  ~680 lines) plus handler files `ReControlCmds_Session.cpp`, `_Input.cpp`,
  `_Query.cpp`, `_Debug.cpp`, `_Profile.cpp` (split 2026-03-13, commit
  9ec9fff).
- **Dispatch:** every command is a table entry (`ReControl.cpp:101`) with an
  explicit threading category — `kCmdImmediate`, `kCmdWorkerDirect`,
  `kCmdWorkerCycle`, `kCmdWorkerSysCall`, `kCmdWorkerRaw`, `kCmdAdaptive`,
  `kCmdCustom`. See `ReControl.h` and `docs/architecture.md` § ReControl.
- **Architecture:** `ReControlServer` (QTcpServer) creates `ReControlSession`
  (QObject per connection).  All I/O on the Qt main thread.  CPU-dependent
  commands dispatch to `CPUWorkerThread` via `QueueWork`/`QueueWorkResult`.
- **Thread safety:** QPointer guards all response lambdas against
  session-destroyed races.  See `docs/stability-findings.md` for full analysis.
- **Input delivery (Phase 2):** `tap`/`tap-id`/`pen`/`key`/`type` are
  `kCmdWorkerRaw` and **delivery-honest** — the handler posts the event, then
  blocks ≤2 s on a per-queue delivery counter that PuppetString increments once
  the event reaches the Palm OS event queue.  `OK delivered` means the guest has
  it; otherwise the response is a truthful `ERR pending`/`ERR busy`/`ERR
  duplicate` (see the Input table).  `button` stays queued (`OK` = enqueued to
  the hardware-button state).  Argument errors are still validated on the
  **main thread** before queueing, so malformed input (wrong arg count,
  non-numeric coordinates, unknown button name) returns `ERR usage`
  immediately rather than a misleading `OK`.
