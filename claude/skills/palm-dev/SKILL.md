---
name: palm-dev
description: Use when developing or testing Palm OS applications in the POSE64 emulator, including using MCP tools, automating emulator actions, or writing Palm app test scripts.
---

# POSE64 Emulator Control

Control the POSE64 Palm OS emulator using native MCP tool calls.

## Architecture

```
Claude Code <-- stdio JSON-RPC --> pose64-mcp-proxy <-- TCP --> pose64:6416
```

- **`pose64`** -- the emulator GUI. Listens for ReControl TCP commands on
  `localhost:6416` by default (`--port <N>` to override, `--no-recontrol` to
  disable). Launch with a session file: `pose64 session.psf`
- **`pose64-mcp-proxy`** -- standalone stdio bridge that translates MCP
  JSON-RPC into ReControl TCP commands. Flags: `--host HOST` (default
  `127.0.0.1`), `--port PORT` (default `6416`). Both binaries are installed
  to `$PATH` by the system package.

## Setup

Add the MCP server to Claude Code (one-time, per-project or global):

```bash
# Per-project: create .mcp.json in project root
echo '{ "mcpServers": { "pose64": { "type": "stdio", "command": "pose64-mcp-proxy" } } }' > .mcp.json

# Or global: add to ~/.claude/mcp.json
```

Then launch the emulator with a session:

```bash
pose64 /path/to/session.psf &
```

Once both are running, `palm_*` MCP tools are available -- call them directly.

## MCP Tools

| Tool | Parameters | Description |
|------|-----------|-------------|
| `palm_ping` | -- | Test MCP connectivity |
| `palm_state` | -- | Emulator state + device info (JSON) |
| `palm_ui` | -- | Active form structure with object IDs |
| `palm_apps` | -- | Installed applications (JSON array) |
| `palm_tap` | `x`, `y` | Tap at display coordinates |
| `palm_tap_id` | `id` | Tap form object by stable ID |
| `palm_pen` | `action`, `x`, `y` | Raw pen down/up |
| `palm_key` | `code` | Key event by character code |
| `palm_type` | `text` | Type full text string |
| `palm_button` | `name`, `action` | Hardware button (power/up/down/app1-4) |
| `palm_screenshot` | `path`, `scale`, `grid`, `annotate`, `crosshair` | Save PNG or return base64 image (see below) |
| `palm_screen_hash` | -- | Screen CRC32 hash + dimensions |
| `palm_launch` | `app` | Launch app by database name |
| `palm_install` | `path` | Install .prc/.pdb file |
| `palm_export` | `db`, `path` | Export database as .prc/.pdb file |
| `palm_save` | `path` | Save session state |
| `palm_load` | `path` | Load session (replaces current) |
| `palm_reset` | `type` (optional) | Reset device (soft/hard/debug) |
| `palm_sleep` | `ms` | Wait N milliseconds |
| `palm_dialog` | `respond` (optional) | Query or dismiss a pending modal dialog |
| `palm_quit` | -- | Exit emulator |
| `palm_run` | `script` | Batch commands separated by `;` (see below) |
| `palm_peek` | `addr`, `nbytes` | Read bytes from emulated memory |
| `palm_poke` | `addr`, `nbytes`, `data` | Write bytes to emulated memory |
| `palm_regs` | -- | Read m68k CPU registers (D0-D7, A0-A7, PC, SR) |
| `palm_menu` | `menu`, `item` | Trigger a menu item by menu and item title |
| `palm_dbs` | -- | List all databases (apps, data, overlays, libraries, etc.) |
| `palm_delete` | `db` | Delete a database from the device |

All tools return structured text. No Bash calls needed for emulator interaction.

## Efficiency Patterns

### Use `palm_ui` first, `palm_screenshot` second

`palm_ui` returns structured text describing every form object: buttons with IDs
and labels, fields with text content, lists with items. This is far more
informative than a screenshot and requires no image decoding.

Only use `palm_screenshot` when you need to verify visual layout or see content
that `palm_ui` doesn't capture (like graphics or custom-drawn views).

### Screenshot annotation overlays

When you do need a screenshot, use the overlay parameters to get accurate
coordinate information instead of guessing from a tiny 160x160 image:

| Parameter | Type | Description |
|-----------|------|-------------|
| `path` | string | File path to save PNG (optional; omit to get base64 inline) |
| `scale` | int | Upscale factor, e.g. 4 for 640x640 (default 1, max 16) |
| `grid` | bool | Draw coordinate rulers and gridlines every 10/20 Palm pixels |
| `annotate` | bool | Draw colored bounding boxes with IDs from `palm_ui` data |
| `crosshair` | string | Draw red crosshair at `"x,y"` coordinates, e.g. `"80,72"` |

```
# AI-friendly annotated screenshot (recommended for visual inspection)
palm_screenshot scale=4 grid=true annotate=true

# Probe a specific coordinate
palm_screenshot scale=4 crosshair="80,72"

# Raw screenshot (old behavior, no overlays)
palm_screenshot
```

When `annotate=true`, the response includes both the image and `palm_ui`
structured text in a single call. Color coding: blue=buttons, green=fields,
orange=lists, purple=gadgets, yellow=scrollbars, cyan=titles, gray=labels.

The CRC hash returned is always computed on raw pixels (before overlays),
so `palm_screen_hash` comparisons remain stable regardless of overlay options.

### Use `palm_screen_hash` to skip redundant screenshots

After any action, check the hash before taking a screenshot:

```
palm_screen_hash  ->  hash1
palm_tap 80 80
palm_screen_hash  ->  hash2
# Only screenshot if hash1 != hash2 (screen changed)
```

### Use `palm_tap_id` instead of coordinates

Object IDs are stable across screen sizes and layout changes. Coordinates break.

```
# BAD: hardcoded coordinates
palm_tap x=44 y=153

# GOOD: tap by object ID (from palm_ui output)
palm_tap_id id=1015
```

### Use `palm_type` for text entry

One call instead of N `palm_key` calls:

```
# BAD: character by character
palm_key code=72   # H
palm_key code=101  # e

# GOOD: one call
palm_type text="Hello"
```

### Use `palm_ui` to verify actions

After tapping a button, read `palm_ui` to confirm the expected form appeared:

```
palm_tap_id id=1015     # tap "New" button
palm_sleep ms=500
palm_ui                 # check what form appeared -- verify expected dialog
```

### Handle modal dialogs with `palm_dialog`

When the emulator hits an error (illegal instruction, ROM warning, debugger break),
it shows a modal dialog and the CPU blocks (`blocked_on_ui` in `palm_state`).
Use `palm_dialog` to inspect and dismiss these programmatically:

```
palm_state              -> "blocked_on_ui" means a dialog is pending
palm_dialog             -> returns message, buttons, AND CPU registers (crash diagnostics)
palm_dialog respond=continue   -> dismisses the dialog, CPU resumes
palm_state              -> should be "running" again
```

The dialog query response includes a `regs` line with PC, SR, and all data/address
registers when the CPU is blocked.  This gives crash diagnostics inline without
needing a separate `palm_regs` call.

Common button names: `ok`, `cancel`, `continue`, `debug`, `reset`, `yes`, `no`.
Omit `respond` to just query. If no dialog is pending, returns `OK none`.

### Error recovery with `palm_reset`

`palm_reset` works unconditionally — even in `blocked_on_ui` state.  It
dismisses any pending dialog, unblocks the CPU thread, and performs the reset.
Use `palm_reset type=hard` after a crash to fully restore the device.

```
palm_state              -> "blocked_on_ui"
palm_reset type=hard    -> "OK reset (was blocked_on_ui, dialog dismissed)"
palm_state              -> "running"
```

### Crash debugging workflow

When your app crashes (illegal instruction, bus error, etc.):

1. `palm_dialog` — see the error message AND CPU registers (including PC)
2. `palm_regs` — works in `blocked_on_ui`, returns full register dump
3. `palm_peek addr="0x<PC>" nbytes=16` — read code at crash site
4. `palm_dialog respond=reset` or `palm_reset type=hard` — recover

`palm_regs` and `palm_peek` both work while the CPU is blocked because the
CPU state is frozen and stable.

### Loading sessions programmatically

`palm_load` works even from a cold start (no existing session required):

```
palm_load path="/path/to/session.psf"   -> opens session, starts CPU
```

This is essential for automated workflows where the emulator is restarted
without human intervention to open a session via the GUI.

## App-Specific Workflows

See `claude/skills/palm-dev/references/builtin-apps.md` for detailed,
tested workflows for each built-in Palm OS app (To Do List, Date Book, etc.).
These include stable object IDs, step-by-step sequences, and gotchas
discovered through live testing.

## Batch Commands with `palm_run`

Execute multiple commands in a single MCP call, eliminating round-trip latency:

```
palm_run script="tap 12 148; sleep 150; type Hello; tap 12 148; sleep 150"
```

Supported sub-commands: `tap`, `pen`, `key`, `type`, `button`, `sleep`.
Use `repeat N { ... }` for loops:

```
palm_run script="repeat 5 { tap_id 1005; sleep 500; type Item; sleep 300 }"
```

This is the most efficient way to perform mechanical UI sequences.

## Memory Inspection

Read and write emulated device memory directly, useful for verifying patches
or inspecting app state without navigating the UI.

**Address formats:**
- `0x00012345` -- absolute hex address
- `a5@-6423` -- A5-relative signed decimal offset (app globals)
- `global.uiCurrentMenu` -- named low-memory global

```
palm_peek addr="a5@-6423" nbytes=2     # read 2 bytes from app global
palm_poke addr="0x1234" nbytes=1 data="FF"   # write 1 byte
palm_regs                               # dump all CPU registers
```

Max 256 bytes per peek/poke. Data is hex-encoded.

## Debugging Commands

### Stack Trace

```
palm_backtrace                        # or palm_bt — stack crawl
```

Works in `blocked_on_ui` for crash analysis. Returns PC and A6 per frame.

### Breakpoints (6 slots, indices 0-5)

```
palm_break list                       # show all breakpoint slots
palm_break set 0 0x12340              # set breakpoint at address
palm_break set 0 0x12340 d5.w==0x1234 # with condition
palm_break clear 0                    # remove breakpoint
palm_break enable 0                   # enable
palm_break disable 0                  # disable
```

### Data Watchpoints

```
palm_watch set 0x12340 16             # watch 16 bytes at address
palm_watch clear                      # remove watchpoint
palm_watch status                     # query state
palm_spy set 0x12340                  # monitor single address for changes
palm_spy clear
palm_spy status
```

### Logging (20 categories)

```
palm_log list                         # show categories with levels
palm_log set SystemCalls 2            # 0=off, 1=gremlin-only, 2=always
palm_log dump                         # flush buffer to file
palm_log clear                        # clear buffer
```

### Gremlins (Automated Stress Testing)

```
palm_gremlin new 42 10000            # seed=42, run 10000 events
palm_gremlin status                  # query progress
palm_gremlin suspend / step / resume / stop
```

### Memory Checks (18 flags)

```
palm_check list                      # show all flags
palm_check set FreeChunkAccess on    # enable specific check
palm_check set-all on                # enable all checks
```

### Error Handling

```
palm_errorhandling get               # show behavior settings
palm_errorhandling set WarningOff continue  # auto-continue warnings
```

Settings: `WarningOff`, `ErrorOff`, `WarningOn`, `ErrorOn`.
Options: `show`, `continue`, `quit`, `switch`.

### Profiling

```
palm_profile init                    # initialize (optional: maxcalls maxdepth)
palm_profile start                   # begin collecting
palm_profile stop                    # pause collecting
palm_profile dump /tmp/profile.mwp   # write Metrowerks profile
palm_profile print /tmp/profile.txt  # write text report
palm_profile cleanup                 # free profiler
palm_profile cycles                  # read cycle counters
```

## Triggering Menus by Name

Palm OS menus are difficult to interact with via coordinates (pen-drag-release).
Use `palm_menu` to trigger a menu item directly by title:

```
palm_menu menu="Record" item="Delete Item..."
palm_menu menu="Options" item="About"
```

Item matching is case-insensitive and supports substring matching.
The menu bar is auto-activated if not already open -- no need to press
the menu key first. This posts a `menuEvent` to the Palm OS event queue --
the app processes it as if the user selected the menu item normally.

**Discovering available menus:** `palm_ui` only includes menu bar contents
(MENUBAR/MENU/ITEM lines) when the menu bar is currently open. If you need
to enumerate available menus and items, first open the menu bar with
`palm_key code=261`, then call `palm_ui` to read the full menu structure.
Normal `palm_ui` output (menu bar closed) will NOT show any menu information.

## Deleting Databases

Remove an installed app or database before re-installing:

```
palm_delete db="Shadow"
palm_install path="/path/to/Shadow.prc"
```

This replaces the manual UI workflow of Launcher -> App menu -> Delete.

## Navigation and Key Codes

**IMPORTANT: Always use key codes for system actions.** The Palm OS
silkscreen area (below the display) contains buttons for Home, Menu,
Find, and Calculator, but their pixel positions vary by device and skin.
NEVER try to guess or tap silkscreen coordinates. Instead, use the
corresponding `palm_key` codes, which work reliably on all devices:

### Going Home (Launcher)

```
palm_launch app="Launcher"     # switch to the app launcher (preferred)
palm_key code=264              # vchrLaunch (0x0108) -- also goes home
```

### Useful Virtual Character Codes

These are sent via `palm_key code=N`. Virtual chars (>= 256) trigger
system actions; printable chars (< 128) are typed as text.

| Code | Name | Effect |
|------|------|--------|
| 8 | Backspace | Delete character before cursor |
| 10 | Newline/Enter | Line break in text fields (does NOT submit forms) |
| 11 | Page Up | Scroll up in lists/text (same as hardware Up button) |
| 12 | Page Down | Scroll down in lists/text (same as hardware Down button) |
| 28 | Left Arrow | Move cursor left |
| 29 | Right Arrow | Move cursor right |
| 30 | Up Arrow | Move cursor/selection up |
| 31 | Down Arrow | Move cursor/selection down |
| 259 | Next Field | Tab to next field in form (vchrNextField) |
| 261 | Menu | Activate menu bar (vchrMenu) -- prefer `palm_menu` instead |
| 264 | Launch | Go to Launcher / Home (vchrLaunch) |
| 266 | Find | Open global Find dialog (vchrFind) |
| 267 | Calculator | Open Calculator (vchrCalc) |
| 268 | Prev Field | Tab to previous field in form (vchrPrevField) |

### Hardware Buttons

Sent via `palm_button name=<name> action=<tap|down|up>`:

| Name | Default Mapping |
|------|----------------|
| `app1` | Date Book |
| `app2` | Address Book |
| `app3` | To Do List |
| `app4` | Note Pad |
| `up` | Page Up / Previous |
| `down` | Page Down / Next |
| `power` | Power on/off |
| `cradle` | HotSync cradle button |
| `contrast` | Contrast adjustment |

### Scrolling

```
palm_button name=up action=tap     # page up in current view
palm_button name=down action=tap   # page down in current view
palm_key code=11                   # same as hardware Up (pageUp)
palm_key code=12                   # same as hardware Down (pageDown)
```

## Anti-Patterns (Do NOT)

- **Do NOT tap silkscreen areas to open menus, go home, or find** -- the silkscreen
  is below the 160x160 display area and coordinates vary by device/skin. Use key
  codes instead: `palm_key code=261` (menu), `palm_key code=264` (home),
  `palm_key code=266` (find). Or better: `palm_menu` for menus, `palm_launch`
  for switching apps.
- **Do NOT try to read menu contents from `palm_ui` without opening the menu first** --
  `palm_ui` only includes MENUBAR/MENU/ITEM lines when the menu bar is open.
  To discover menus: `palm_key code=261` then `palm_ui`. To trigger an item
  you already know by name: just call `palm_menu` directly.
- **Do NOT screenshot after every action** -- use `palm_screen_hash` or `palm_ui` instead
- **Do NOT hardcode pixel coordinates** -- use `palm_tap_id` or read coordinates from `palm_ui`
- **Do NOT use `palm_sleep` for synchronization** -- use `palm_ui` to poll for expected form state
- **Do NOT type character by character** -- use `palm_type`
- **Do NOT parse screenshots for text** -- use `palm_ui` which returns structured text directly
