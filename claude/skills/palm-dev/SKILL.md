---
name: palm-dev
description: Use when developing or testing Palm OS applications in the POSE64 emulator, including interacting with ReControl TCP interface, automating emulator actions, or writing Palm app test scripts.
---

# ReControl: POSE64 TCP Control Interface

ReControl is a text-based TCP protocol for controlling the POSE64 Palm OS emulator. One command per line, `\n` terminated. One active connection at a time.

## Connecting

Use `scripts/rc.py` for one-shot commands from Bash:

```bash
python3 scripts/rc.py --port 6416 state
python3 scripts/rc.py --port 6416 ui
python3 scripts/rc.py --port 6416 tap-id 1005
```

For multi-command sessions, use `ReControlClient` from `test_recontrol.py`:

```python
from test_recontrol import ReControlClient
c = ReControlClient(port=6416, timeout=5)
c.connect()
resp = c.send_command("state")
c.disconnect()
```

**Port conventions:** 6416 (dev), 6425 (secondary), 6427 (automated testing).

## Command Reference

### State Commands

| Command | Response | Description |
|---------|----------|-------------|
| `state` | `OK running\n` | Emulator state (running/suspended/stopped) |
| `info` | Multi-line, `.` terminated | Device info, screen size, ROM, session path |
| `apps` | Multi-line, `.` terminated | List installed applications with type/creator |

### Input Commands

| Command | Response | Description |
|---------|----------|-------------|
| `tap <x> <y>` | `OK\n` | Pen down+up at display coordinates |
| `tap-id <id>` | `OK <x> <y>\n` | Tap center of form object by ID |
| `pen <down\|up> <x> <y>` | `OK\n` | Pen down or up separately |
| `key <charcode>` | `OK\n` | Single key event |
| `type <text>` | `OK\n` | Type full text string (Latin-1) |
| `button <name> <down\|up\|tap>` | `OK\n` | Hardware button (power/up/down/app1-4) |

### Screen Commands

| Command | Response | Description |
|---------|----------|-------------|
| `screenshot <path>` | `OK <hash> <w> <h>\n` | Save PNG, returns CRC32 hash |
| `screen-hash` | `OK <hash> <w> <h>\n` | Screen CRC32 without saving file |
| `ui` | Multi-line, `.` terminated | Active form structure with object IDs/bounds |

### Session Commands

| Command | Response | Description |
|---------|----------|-------------|
| `save <path>` | `OK\n` | Save session state |
| `load <path>` | `OK\n` | Load session state (destroys current) |
| `install <path>` | `OK\n` | Install .prc/.pdb file |
| `launch <dbname>` | `OK\n` | Launch app by database name |
| `reset [soft\|hard\|debug]` | `OK\n` | Reset emulator |
| `sleep <ms>` | `OK\n` | Pause command processing (1-30000ms) |
| `quit` | `OK\n` | Exit emulator |

### Multi-line Response Protocol

Commands `ui`, `info`, and `apps` return multi-line responses terminated by a line containing only `.`. Use `latin-1` decoding (Palm OS text is not UTF-8).

## Efficiency Patterns

### Use `ui` first, `screenshot` second

The `ui` command returns structured text describing every form object: buttons with IDs and labels, fields with text content, lists with items. This is far more informative than a screenshot and requires no image decoding.

```bash
# GOOD: Read the form structure
python3 scripts/rc.py ui
# Output: OK FORM id=1000 "Date Book"
#   BUTTON id=1015 "New" (44,147,22,12)
#   FIELD id=1109 (0,16,153,121) "Meeting notes..."
```

Only use `screenshot` when you need to verify visual layout or see content that `ui` doesn't capture (like graphics or custom-drawn views).

### Use `screen-hash` to skip redundant screenshots

After any action, check the hash before taking a screenshot:

```python
hash1 = c.send_command("screen-hash")  # "OK a3f7c012 160 160"
c.send_command("tap 80 80")
hash2 = c.send_command("screen-hash")  # same hash? screen didn't change
if hash1 != hash2:
    c.send_command("screenshot /tmp/screen.png")  # only capture if changed
```

### Use `tap-id` instead of coordinates

Object IDs are stable across screen sizes and layout changes. Coordinates break.

```bash
# BAD: hardcoded coordinates
python3 scripts/rc.py tap 44 153

# GOOD: tap by object ID (from ui output)
python3 scripts/rc.py tap-id 1015
```

### Use `type` for text entry

One command instead of N `key` commands:

```bash
# BAD: character by character
python3 scripts/rc.py key 72   # H
python3 scripts/rc.py key 101  # e
python3 scripts/rc.py key 108  # l
python3 scripts/rc.py key 108  # l
python3 scripts/rc.py key 111  # o

# GOOD: one command
python3 scripts/rc.py type Hello
```

### Use `ui` to verify actions

After tapping a button, read `ui` to confirm the expected form appeared:

```python
c.send_command("tap-id 1015")      # tap "New" button
time.sleep(0.5)
ui = read_multiline(c, "ui")       # check what form appeared
assert "Set Time" in ui            # verify expected dialog
```

## App-Specific Workflows

See `claude/skills/palm-dev/references/builtin-apps.md` for detailed,
tested workflows for each built-in Palm OS app (To Do List, Date Book, etc.).
These include stable object IDs, step-by-step command sequences, and gotchas
discovered through live testing.

## Anti-Patterns (Do NOT)

- **Do NOT screenshot after every action** — use `screen-hash` or `ui` instead
- **Do NOT hardcode pixel coordinates** — use `tap-id` or read coordinates from `ui`
- **Do NOT use `sleep` for synchronization** — use `ui` to poll for expected form state
- **Do NOT type character by character** — use `type`
- **Do NOT parse screenshots for text** — use `ui` which returns structured text directly
