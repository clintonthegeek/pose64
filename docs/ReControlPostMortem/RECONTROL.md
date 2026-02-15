# ReControl: Unix Socket Control Interface for RePOSE4

## Overview

ReControl adds a programmatic control interface to RePOSE4 (our native build of
POSE 3.5) via a Unix domain socket. It exposes emulator internals — screen
capture, memory access, event injection, PRC installation, app launching, CPU
register inspection — through a simple line-based text protocol.

**Purpose:** Replace the manual CloudpilotEmu + ydotool workflow for testing
ShadowStan (our clean-room reimplementation of ShadowPlan 4.31). With ReControl,
a script or Claude can build ShadowStan, install it, launch it, interact with it,
and inspect its state — all without touching a browser or GUI.

**Status:** Fully functional. Phase 1 (basic commands) built and tested 2025-02-14.
Phase 2 (trap tracing + socket breakpoints) built and tested 2026-02-14. PNG
screenshots, memory peek/poke, register dumps, PRC install, app launch by DB name,
pen/key/button events, trap call tracing, instruction breakpoints, single-stepping,
and resume all working.

---

## Files Changed

### New files

| File | Lines | Description |
|------|-------|-------------|
| `src/SrcUnix/ReControl.h` | ~15 | Public API: `ReControl_Startup(path)`, `ReControl_Shutdown()` |
| `src/SrcUnix/ReControl.cpp` | ~1250 | Socket listener, command handlers, PNG encoder |
| `src/SrcShared/ReControlTrace.h` | ~50 | Trace + breakpoint API declarations |
| `src/SrcShared/ReControlTrace.cpp` | ~380 | Trap tracing, breakpoint state, step implementation |

### Modified files

| File | What changed |
|------|-------------|
| `src/SrcUnix/EmApplicationFltk.cpp` | Added `#include "ReControl.h"` and `#include <cstring>`. Modified `main()` to parse `--sock PATH` from argv (strips it before FLTK/POSE see it), call `ReControl_Startup(sockPath)` after `theApp.Startup()`, and `ReControl_Shutdown()` before `theApp.Shutdown()`. |
| `src/SrcShared/EmPalmOS.cpp` | Added `#include "ReControlTrace.h"`. Added 4-line trace hook in `HandleSystemCall()` after the `LogSystemCalls` block: checks `gReTraceEnabled` and calls `ReControlTrace_Hook(context)`. |
| `src/SrcShared/EmSession.h` | Added `ResumeFromDebugger()` and `ForceResume()` method declarations. |
| `src/SrcShared/EmSession.cpp` | Added `ResumeFromDebugger()` (~15 lines) and `ForceResume()` (~15 lines). Fixed `ExecuteSubroutine` race condition with `fSuspendByUIThread` counter accumulation. |
| `Makefile` | Added `ReControl` to `SRC_UNIX`, `ReControlTrace` to `SRC_SHARED`. |

---

## Command Line

```
./build/pose -rom <romfile> [--sock <path>] [other POSE flags...]
```

| Flag | Default | Description |
|------|---------|-------------|
| `--sock <path>` | `/tmp/repose.sock` | Unix domain socket path for ReControl. If omitted, ReControl still starts on the default path. |

The `--sock` flag is parsed and stripped from argv before POSE/FLTK process
arguments, so it doesn't interfere with POSE's own `-rom`, `-device`, etc. flags.

**Important:** POSE shows a config dialog on first launch (ROM selection, device,
skin). You must interact with this dialog before the emulator session starts and
the socket becomes useful. Once a session `.psf` file exists, subsequent launches
with `-psf <file>` skip the dialog.

---

## Socket Protocol

**Transport:** Unix domain socket, SOCK_STREAM.

**Framing:** Line-based text. Each command is a single line terminated by `\n`.
Each response is a single line terminated by `\n`.

**Request format:**
```
COMMAND [arg1] [arg2] ...\n
```

**Response format:**
```
OK [optional data]\n
```
or
```
ERR <message>\n
```

Commands are case-insensitive. Arguments are space-delimited.

**Connection model:** The socket accepts one client at a time. Multiple commands
can be sent on a single connection (one per line). The connection stays open until
the client disconnects. New connections are accepted after the previous one closes.

---

## Commands

### install \<filepath\>

Load a PRC/PDB file into the emulated Palm device.

**Threading:** Stops CPU at syscall boundary (`kStopOnSysCall`), calls
`EmFileImport::LoadPalmFile` with `kMethodBest`, then resumes.

```
-> install /home/clinton/dev/palmtest/shadow-re/src/ShadowStan.prc
<- OK installed dbID=0x00040333
```

**Errors:** File not found, file too large (>4MB), empty file, session not
running, LoadPalmFile failure, emulator exception during ROM call (e.g. if
the ROM throws `EmExceptionReset` during `DmNewResource`).

---

### launch \<dbname\>

Launch an installed application by its PalmOS database name.

**Threading:** Stops CPU at syscall boundary (`kStopOnSysCall`), calls
`DmFindDatabase(0, name)` via ROMStubs to look up the database, then calls
`SysUIAppSwitch(0, dbID, sysAppLaunchCmdNormalLaunch, NULL)` to trigger the
app switch. The switch happens asynchronously when PalmOS processes its next
event loop iteration.

```
-> launch ShadowStan
<- OK
```

The `dbname` must be the exact PalmOS database name as stored in the PRC header
(case-sensitive). For ShadowStan, this is `"ShadowStan"`. For built-in apps:
`"Address"`, `"Calc"`, `"Date Book"`, `"Expense"`, `"Mail"`, `"Memo Pad"`,
`"Note Pad"`, `"Security"`, `"To Do List"`, etc.

**Errors:** Database not found, session not running, cannot stop at syscall
boundary, SysUIAppSwitch failure, emulator exception during ROM call (e.g.
if the ROM throws `EmExceptionReset` during `DmFindDatabase`).

---

### tap \<x\> \<y\>

Simulate a pen tap (down + up) at LCD coordinates.

**Threading:** Direct call to `gSession->PostPenEvent()` — thread-safe, no CPU
stop needed.

```
-> tap 80 80
<- OK
```

Coordinates are in Palm LCD pixel space: (0,0) is top-left of the touchscreen.
Typical screen sizes: 160x160 (m500, m505) or 160x240 (some later devices).

The pen-up event uses coordinates (-1, -1) which is the Palm convention for
"pen lifted."

---

### pen \<down|up\> \<x\> \<y\>

Post a raw pen event (either down or up separately). Use this for drags.

```
-> pen down 80 80
<- OK
-> pen up 80 80
<- OK
```

---

### key \<charcode\>

Enqueue a key event. The charcode is a decimal integer — either an ASCII value
or a Palm virtual character code.

**Threading:** Direct call to `gSession->PostKeyEvent()` — thread-safe.

```
-> key 264
<- OK
```

**Useful virtual character codes** (from `Chars.h`):

| Code | Name | Description |
|------|------|-------------|
| 264 | `vchrLaunch` (0x0108) | Go to app launcher |
| 261 | `vchrMenu` (0x0105) | Open menu bar |
| 265 | `vchrCommand` (0x0109) | Command stroke |
| 266 | `vchrConfirm` (0x010A) | Confirm/OK |
| 4 | `chrLineFeed` | Enter/Return |
| 8 | `chrBackspace` | Backspace |
| 518 | `vchrFind` (0x0206) | Global Find |

---

### button \<name\> \<down|up\>

Simulate a hardware button press or release.

**Threading:** Direct call to `gSession->PostButtonEvent()` — thread-safe.

```
-> button app1 down
<- OK
-> button app1 up
<- OK
```

**Button names and what they do on a typical Palm m500:**

| Name | Palm button | Launches |
|------|-------------|----------|
| `power` | Power | Toggle power |
| `up` | Up rocker | Scroll up / Page up |
| `down` | Down rocker | Scroll down / Page down |
| `app1` | Hard button 1 | Date Book |
| `app2` | Hard button 2 | Address |
| `app3` | Hard button 3 | To Do List |
| `app4` | Hard button 4 | Memo Pad |
| `cradle` | HotSync button | Start HotSync |
| `contrast` | Contrast | Adjust contrast |
| `antenna` | Antenna | Toggle wireless |

---

### screenshot \<filepath\>

Save the current LCD contents as a PNG file.

**Threading:** Stops CPU (`kStopNow`), captures screen via
`EmScreen::InvalidateAll()` + `EmScreen::GetBits()`, converts pixel data to
24-bit RGB, deflate-compresses with `GzipEncode`, wraps in PNG format, resumes
CPU, writes file to disk.

```
-> screenshot /tmp/screen.png
<- OK 160 160
```

Response includes the image dimensions (width height). The file is a standard
24-bit RGB PNG that can be viewed with any image viewer or Claude's Read tool.

**PNG encoding details:** The encoder is self-contained within ReControl.cpp:
- CRC-32 table computed at runtime (standalone, doesn't use gzip's `updcrc`)
- Pixel data filtered with filter type 0 (None) for each scanline
- Compressed using POSE's built-in deflate library (`GzipEncode` from Miscellaneous.h)
- Wrapped in zlib format (0x78 0x01 header + raw deflate + Adler-32 trailer)
- Typical output size: ~1KB for a 160x160 Palm screen (vs ~77KB for BMP)

**Thread safety note:** `GzipEncode` uses global state, so compression is
performed while the CPU is stopped to prevent concurrent access.

---

### sleep \<ms\>

Pause the socket connection for the specified number of milliseconds. During
this time the emulator CPU continues running and processing events. Use this
between `tap` and `screenshot` to let UI animations complete.

```
-> tap 80 80
-> sleep 1000
-> screenshot /tmp/after_tap.png
```

Maximum: 30000 ms (30 seconds). The `OK` response is sent after the sleep
completes.

```
-> sleep 500
<- OK
```

---

### peek \<hexaddr\> \<len\>

Read bytes from emulated memory. Returns hex-encoded data.

**Threading:** Stops CPU at cycle boundary (`kStopOnCycle`), uses
`CEnableFullAccess` to bypass access checks, reads via `EmMemGet8`, resumes.

```
-> peek 00000000 16
<- OK ffffffff0000000010015ba810015bc6
```

Address is hex (without `0x` prefix). Length is decimal (1-65536).

---

### poke \<hexaddr\> \<hexbytes\>

Write bytes to emulated memory.

**Threading:** Same as peek — stops CPU, uses `CEnableFullAccess`, writes via
`EmMemPut8`.

```
-> poke 00012340 deadbeef
<- OK wrote 4 bytes
```

---

### regs

Dump all m68k CPU registers.

**Threading:** Stops CPU at cycle boundary (`kStopOnCycle`), reads via
`gCPU68K->GetRegister()`.

```
-> regs
<- OK d0=00000001 d1=00000004 d2=00000000 d3=00026cdc d4=00026cb0 d5=00026cb0 d6=00000064 d7=00000001 a0=fffff000 a1=0003e6a6 a2=0003e6a6 a3=00038378 a4=0003f356 a5=00004b4e a6=0003eb4e usp=0003eb38 ssp=0003eb38 pc=10115224 sr=2600
```

Registers: d0-d7 (data), a0-a6 (address), usp (user stack pointer), ssp
(supervisor stack pointer), pc (program counter), sr (status register).
All values are 32-bit hex except sr which is 16-bit.

---

### state

Report the emulator session state.

**Threading:** Calls `gSession->GetSessionState()` — has its own mutex, safe
from any thread.

```
-> state
<- OK running
```

**Possible states:**

| State | Meaning |
|-------|---------|
| `running` | CPU is executing normally |
| `stopped` | Session has ended |
| `suspended` | CPU is paused (see below for reason) |
| `blocked_on_ui` | CPU is waiting for a POSE dialog to be dismissed (e.g. error dialog, debug prompt) |

When the session is suspended, `state` appends the primary reason and raw
counter diagnostics:

```
-> state
<- OK suspended:debugger ui=0 dbg=1 ext=0 timeout=0 syscall=0 subret=0
```

**Suspend reason labels** (in priority order):

| Label | Meaning |
|-------|---------|
| `suspended:debugger` | Stopped at a breakpoint or by the debugger |
| `suspended:external` | Suspended by an external request |
| `suspended:timeout` | Execution timeout |
| `suspended:syscall` | Stopped at a syscall boundary (e.g. during install/launch) |
| `suspended:subreturn` | Stopped at subroutine return |
| `suspended:ui` | Stopped by the FLTK UI thread (e.g. screen paint) |
| `suspended:unknown` | Counters are non-zero but no specific reason matched |

The raw counters (`ui=`, `dbg=`, etc.) are always included when any counter
is non-zero, for diagnostic purposes.

**`blocked_on_ui`:** This state means POSE is displaying a modal dialog in
the GUI (typically an error or confirmation dialog like "The emulator
detected an error..." with Reset/Debug/Continue buttons). The CPU thread is
blocked in `BlockOnDialog()` waiting for the user to dismiss it. Socket
commands that need to stop the CPU (`install`, `launch`, `screenshot`, etc.)
will hang until the dialog is dismissed. Use `reset` to recover — it
bypasses the dialog entirely (see below).

---

### reset [soft|hard|debug]

Reset the emulated device. Default is soft reset.

```
-> reset
<- OK
-> reset hard
<- OK
```

| Type | PalmOS equivalent |
|------|-------------------|
| `soft` | Pin in reset hole |
| `hard` | Soft reset + holding Power (erases all data) |
| `debug` | Soft reset + holding Page Down |

**No confirmation dialog:** Unlike the GUI menu path (which shows "Are you
sure?" via `EmDlg::DoReset`), the socket `reset` command calls
`ScheduleReset()` directly and never shows a confirmation dialog. This makes
it safe to use from scripts.

**Recovery from `blocked_on_ui`:** If the emulator is stuck displaying an
error dialog (`state` returns `blocked_on_ui`), `reset` will schedule the
reset. The reset takes effect when the dialog is eventually dismissed (or
the CPU processes the next cycle). For a stuck dialog, combine with
`resume` to force-clear suspend counters first.

---

### quit

Tell POSE to exit. Sets the FLTK quit flag; the main loop will exit on its
next iteration.

```
-> quit
<- OK
```

---

### trace \<filepath\> | trace off

Start or stop trap call tracing. When enabled, every PalmOS API call (system
trap) is logged to the specified file with the trap word, resolved function
name, PC, and first 6 stack words (raw arguments).

**Threading:** The `gReTraceEnabled` flag is checked on the CPU thread's hot
path in `EmPalmOS::HandleSystemCall()`. It's a single branch — zero overhead
when tracing is off. The FILE* and writes are protected by a mutex.

```
-> trace /tmp/trap.log
<- OK
-> trace off
<- OK
```

**Trace output format** (one line per trap call):
```
0xA049 DmOpenDatabase pc=10015ba8 sp+: 00000000 0003a420 00000000 00000000 00000000 00000000
0xA04E DmOpenDatabaseByTypeCreator pc=10015bc6 sp+: 53685370 61706c00 00000003 00000000 00000000 00000000
0xA0AE FrmDrawForm pc=10015c04 sp+: 0003e6a0 00000000 00000000 00000000 00000000 00000000
```

Each field: `0x<trapword> <name> pc=<hex> sp+: <6 stack words as hex>`

The stack words are the raw bytes at SP+0, SP+4, ..., SP+20. For traps that
use stack-based calling conventions (most PalmOS traps), these are the
arguments. Per-trap argument decoding (e.g. knowing that DmOpenDatabase takes
`(UInt16 cardNo, LocalID dbID, UInt16 mode)`) can be added later by
interpreting the raw bytes according to the trap's prototype.

---

### break \<hexaddr\>

Set an instruction breakpoint at the given address. The address must be
even (m68k instruction alignment).

**Implementation:** Calls `MetaMemory::MarkInstructionBreak(addr)` to set
the break bit in meta-memory. The CPU loop checks this bit every instruction
cycle — existing POSE infrastructure, near-zero overhead.

```
-> break 10015ba8
<- OK
```

Maximum 32 breakpoints. Duplicate addresses are silently accepted.

---

### breaks

List all active breakpoints.

```
-> breaks
<- OK 10015ba8 10015bc6
```

Returns `OK none` if no breakpoints are set.

---

### delbreak \<hexaddr\>

Remove a breakpoint.

```
-> delbreak 10015ba8
<- OK
```

Returns `ERR breakpoint not found` if the address wasn't set.

---

### continue

Resume the CPU and block until the next breakpoint is hit. This is a
**blocking command** — the response is not sent until a breakpoint fires.

**Flow:**
1. Socket thread records current PC as skip-once address (to avoid
   re-breaking immediately on the same instruction)
2. Calls `ResumeFromDebugger()` to decrement `fSuspendByDebugger` and
   wake the CPU thread
3. Blocks on a condition variable
4. CPU runs until it hits an instruction break
5. The break handler (running in the CPU thread) sets the hit address,
   calls `ScheduleSuspendException()`, and signals the condition variable
6. Socket thread wakes and returns the response

```
-> continue
<- OK BREAK 10015bc6
```

After the response, the CPU is suspended. You can use `regs`, `peek`,
`screenshot`, etc. to inspect state, then `continue` or `step` again.

**Typical workflow:**
```
-> break 10015ba8
<- OK
-> continue
<- OK BREAK 10015ba8
-> regs
<- OK d0=00000001 d1=... pc=10015ba8 sr=2000
-> step
<- OK BREAK 10015baa
-> continue
<- OK BREAK 10015ba8
```

---

### step

Execute one instruction and return the new PC. Like `continue`, this is a
**blocking command**.

**Implementation:** Sets `SPCFLAG_DOTRACE` directly in the m68k status flags
(rather than using the 2-phase TRACE→DOTRACE mechanism, which would execute
two instructions). Installs a one-shot hook on `kException_Trace`. Resumes
the CPU. After one instruction, the trace exception fires, the hook suspends
the CPU and signals the condition variable.

```
-> step
<- OK BREAK 10015baa
```

The returned address is the PC after the stepped instruction — i.e., the
address of the next instruction that would execute.

---

### resume

Force-resume the CPU by clearing **all** suspend counters and setting the
session state to `kRunning`. This is both a normal debugger resume and an
"unstick" command — it recovers from any stuck-suspended state regardless
of the cause.

Unlike `continue`, this command returns immediately. The CPU resumes in the
background.

```
-> resume
<- OK
```

**When to use `resume` vs `continue`:**
- `continue` — resume and wait for the next breakpoint hit (blocking)
- `resume` — resume and return immediately (non-blocking), clears all
  suspend counters

**Use as recovery ("unstick"):** If `state` shows `suspended` with
unexpected counters (e.g. `ui=4` from a race condition), `resume` will
force all counters to zero and restart the CPU. This replaces the need
for a separate "force-resume" or "unstick" command.

---

### ui

Dump the current Palm OS form structure as structured text. Returns the active
form's ID, title, and all form objects with their types, IDs, bounds, and
type-specific state (labels, values, list items, etc.).

**Threading:** Stops CPU at syscall boundary (`kStopOnSysCall`), reads form
structure via Palm OS Form Manager traps (FrmGetActiveForm, FrmGetObjectType,
FrmGetObjectBounds, CtlGetLabel, FldGetTextPtr, LstGetSelectionText, etc.),
then resumes.

**Response format:** Multi-line. First line is `OK FORM id=N "title"` (or
`OK NONE` if no form is active). Subsequent lines are indented with a space,
one per form object. Terminated by `.` on its own line.

```
-> ui
<- OK FORM id=1000 "ShadowStan"
<-  TITLE "ShadowStan" (0,0,160,15)
<-  BUTTON id=1001 "New" (5,147,30,12)
<-  CHECKBOX id=1005 "Done" (5,20,50,12) val=0
<-  FIELD id=1004 (30,2,120,12) "hello world"
<-  LIST id=1003 (0,16,160,128) sel=2 top=0
<-   [0] "Buy groceries"
<-   [1] "Call dentist"
<-   [2] "Fix ShadowStan"
<-  LABEL id=1006 "Priority:" (10,30)
<-  GRAFFITI (0,160,160,0)
<- .
```

**Object types and their format:**

| Type | Format |
|------|--------|
| TITLE | `TITLE "text" (x,y,w,h)` |
| BUTTON | `BUTTON id=N "label" (x,y,w,h)` — includes pushbutton, repeating |
| CHECKBOX | `CHECKBOX id=N "label" (x,y,w,h) val=0\|1` |
| SELECTOR | `SELECTOR id=N "label" (x,y,w,h)` |
| POPUP_TRIGGER | `POPUP_TRIGGER id=N "label" (x,y,w,h)` |
| SLIDER | `SLIDER id=N (x,y,w,h) val=V min=M max=X` |
| FIELD | `FIELD id=N (x,y,w,h) "text content"` |
| LIST | `LIST id=N (x,y,w,h) sel=S top=T` + indented `[i] "item"` lines |
| LABEL | `LABEL id=N "text" (x,y)` |
| SCROLLBAR | `SCROLLBAR id=N (x,y,w,h) val=V min=M max=X` |
| TABLE | `TABLE id=N (x,y,w,h) cols=C rows=R` |
| POPUP | `POPUP id=N ctrl=C list=L` |
| GADGET | `GADGET id=N (x,y,w,h)` |
| BITMAP | `BITMAP id=N (x,y,w,h)` |
| GRAFFITI | `GRAFFITI (x,y,w,h)` |

The focused object is prefixed with `*`: ` *FIELD id=1004 ...`

Strings are quoted with `"`, internal quotes escaped as `\"`, truncated at
80 chars with `...` suffix. Lists show at most 20 items; additional items
shown as `... (N more)`. Custom-draw lists (with a draw callback instead of
text items) show `(custom-draw)` instead of item lines.

**Use case:** Replace PNG screenshots with structured text for Claude-driven
UI testing. A `ui` response is ~200-500 bytes of text vs ~1KB PNG that expands
to many tokens as vision input.

---

## Threading Model

ReControl runs a dedicated pthread that listens on the Unix socket.

**Thread-safe operations** (called directly from socket thread):
- `PostPenEvent`, `PostKeyEvent`, `PostButtonEvent` — POSE's event queues use
  `EmThreadSafeQueue` with internal locking
- `GetSessionState` — protected by `fSharedLock` mutex
- `ScheduleReset` — sets atomic flags + `CheckAfterCycle`
- `SetTimeToQuit` — sets a simple boolean

**CPU-state operations** (require pausing the CPU first):
- `EmSessionStopper(gSession, kStopNow)` — for screenshots (need pixel data)
- `EmSessionStopper(gSession, kStopOnCycle)` — for memory peek/poke, register
  dumps (need CPU quiescent at instruction boundary)
- `EmSessionStopper(gSession, kStopOnSysCall)` — for PRC install and app launch
  (need CPU at a point where Palm OS trap calls can be made via ROMStubs)

The `EmSessionStopper` is an RAII object: constructor pauses the CPU thread,
destructor resumes it. `Stopped()` returns true if the pause succeeded.
`CanCall()` returns true only for `kStopOnSysCall` and means it's safe to call
Palm OS functions via ROMStubs/EmSubroutine.

**Breakpoint/step operations** (cross-thread synchronization):
- Breakpoint handlers run in the CPU thread. They use
  `ScheduleSuspendException()` to suspend the CPU (increments
  `fSuspendByDebugger`, sets `CheckAfterCycle`).
- The socket thread resumes the CPU via `ResumeFromDebugger()` (decrements
  `fSuspendByDebugger`, broadcasts `fSharedCondition`).
- A pthread mutex + condition variable (`gBPMutex`/`gBPCond`) synchronize
  the hit notification: the CPU thread signals when a break fires, the
  socket thread waits for it.
- While the CPU is suspended at a breakpoint, `EmSessionStopper` operations
  (peek, poke, regs, screenshot) work normally — `SuspendThread(kStopNow)`
  sees `fState == kSuspended` and returns immediately.

**Shutdown:** Both `accept()` and `read()` use `poll()` with a 200ms timeout
so the thread checks `gRunning` regularly. Shutdown completes within 200ms
when the FLTK window is closed — no hanging.

---

## Usage Examples

### Basic: Start POSE, install and launch ShadowStan

```bash
# Start POSE (will show config dialog on first run)
cd ~/dev/palmtest/RePOSE4
./build/pose -rom "~/dev/palmtest/Palm OS Emulator/Palm-m500-4.1-en.rom" \
    --sock /tmp/repose.sock &

# After config dialog is dismissed and PalmOS boots...
# Use a persistent connection for multi-step operations:
socat - UNIX-CONNECT:/tmp/repose.sock <<'EOF'
install /home/clinton/dev/palmtest/shadow-re/src/ShadowStan.prc
launch ShadowStan
sleep 2000
screenshot /tmp/shadowstan.png
EOF
```

### One-shot commands via separate connections

```bash
echo "state" | socat - UNIX-CONNECT:/tmp/repose.sock
echo "screenshot /tmp/screen.png" | socat - UNIX-CONNECT:/tmp/repose.sock
echo "peek 00004b4e 32" | socat - UNIX-CONNECT:/tmp/repose.sock
echo "regs" | socat - UNIX-CONNECT:/tmp/repose.sock
```

### Build-install-screenshot workflow

```bash
# Build
cd ~/dev/palmtest/shadow-re/src && make clean && make

# Install + launch + screenshot in one connection
socat - UNIX-CONNECT:/tmp/repose.sock <<'EOF'
install /home/clinton/dev/palmtest/shadow-re/src/ShadowStan.prc
launch ShadowStan
sleep 2000
screenshot /tmp/test.png
EOF

# View the result
# (Claude can use the Read tool on the PNG directly)
```

### Memory inspection for RE work

```bash
# Read 64 bytes starting at some known globals address
echo "peek 00012340 64" | socat - UNIX-CONNECT:/tmp/repose.sock
# -> OK 0000001a004f...

# Write a test value
echo "poke 00012340 deadbeef" | socat - UNIX-CONNECT:/tmp/repose.sock

# Check CPU state
echo "regs" | socat - UNIX-CONNECT:/tmp/repose.sock
```

### Trap tracing for reverse engineering

```bash
# Start tracing, exercise the app, stop tracing
socat - UNIX-CONNECT:/tmp/repose.sock <<'EOF'
trace /tmp/trap.log
launch ShadowStan
sleep 3000
trace off
EOF

# Inspect the trace log
head -20 /tmp/trap.log
```

### Breakpoint debugging session

Use an interactive socat session for breakpoint work (responses come back
inline):

```bash
socat READLINE UNIX-CONNECT:/tmp/repose.sock
# You're now in an interactive session. Type commands:
break 10015ba8
continue
# (CPU runs... eventually hits the breakpoint)
# <- OK BREAK 10015ba8
regs
# <- OK d0=... pc=10015ba8 ...
peek 10015ba8 8
# <- OK 4e714e71...
step
# <- OK BREAK 10015baa
step
# <- OK BREAK 10015bac
continue
# <- OK BREAK 10015ba8
delbreak 10015ba8
resume
# <- OK (CPU runs freely, returns immediately)
```

---

## Architecture Notes

### How ReControl hooks into POSE

```
main() in EmApplicationFltk.cpp
  |
  +-- parse --sock from argv (strips it so FLTK doesn't see it)
  +-- theApp.Startup(argc, argv)  // POSE init, config dialog, session create
  +-- ReControl_Startup(sockPath)  // creates socket, spawns listener pthread
  +-- theApp.Run()                 // FLTK event loop (Fl::wait + HandleIdle)
  +-- ReControl_Shutdown()         // sets gRunning=false, joins thread, unlinks socket
  +-- theApp.Shutdown()            // POSE cleanup
```

### Key POSE APIs used by ReControl

| API | Header | Used for |
|-----|--------|----------|
| `gSession->PostPenEvent()` | EmSession.h | Tap, pen events |
| `gSession->PostKeyEvent()` | EmSession.h | Key events |
| `gSession->PostButtonEvent()` | EmSession.h | Hardware buttons |
| `gSession->GetSessionState()` | EmSession.h | State query |
| `gSession->ScheduleReset()` | EmSession.h | Device reset |
| `EmSessionStopper` | EmSession.h | Pausing CPU for safe operations |
| `EmScreen::InvalidateAll()` | EmScreen.h | Force full screen redraw |
| `EmScreen::GetBits()` | EmScreen.h | Capture LCD pixels |
| `EmPixMap::ConvertToFormat()` | EmPixMap.h | Convert to 24-bit RGB |
| `EmFileImport::LoadPalmFile()` | EmFileImport.h | PRC/PDB installation |
| `EmMemGet8()` / `EmMemPut8()` | EmMemory.h | Memory read/write |
| `CEnableFullAccess` | EmMemory.h | Bypass memory access checks |
| `gCPU68K->GetRegister()` | EmCPU68K.h | CPU register access |
| `DmFindDatabase()` | ROMStubs.h | Find DB by name (Palm OS trap) |
| `SysUIAppSwitch()` | ROMStubs.h | Launch app (Palm OS trap) |
| `GzipEncode()` | Miscellaneous.h | Deflate compression for PNG |
| `gApplication->SetTimeToQuit()` | EmApplication.h | Clean shutdown |
| `GetTrapName()` | EmPalmFunction.h | Resolve trap word to name |
| `MetaMemory::MarkInstructionBreak()` | MetaMemory.h | Set breakpoint bit in meta-memory |
| `gSession->AddInstructionBreakHandlers()` | EmSession.h | Register break installer/remover/reacher |
| `gSession->ScheduleSuspendException()` | EmSession.h | Suspend CPU from CPU thread |
| `gSession->ResumeFromDebugger()` | EmSession.h | Resume CPU from external thread |
| `gSession->ForceResume()` | EmSession.h | Force-clear all suspend counters and resume |
| `gCPU68K->InstallHookException()` | EmCPU68K.h | Hook trace exception for step |

---

## Known Issues and Caveats

1. **Config dialog on first launch:** POSE shows a dialog asking for ROM, device,
   and skin selection. The socket is created before the dialog, but most commands
   will fail (no session) until the dialog is dismissed and PalmOS boots. Use a
   `.psf` session file (`-psf <file>`) to skip the dialog on subsequent launches.

2. **Single client at a time:** The listener accepts one connection. If a second
   client connects, it waits until the first disconnects. This is by design —
   concurrent commands could interfere with each other (especially CPU-stopping
   operations).

3. **GzipEncode thread safety:** The deflate library uses global state. Screenshots
   perform compression while the CPU is stopped to prevent conflicts. This means
   the CPU is paused for slightly longer than strictly necessary (~1ms for a
   160x160 screen).

4. **launch requires syscall boundary:** The `launch` command stops the CPU at
   `kStopOnSysCall`, which requires the CPU to reach a system function call. If
   PalmOS is in a tight loop or crashed, this may fail. In practice this works
   reliably when PalmOS is idle or running normally.

5. **No --sock without a session:** If POSE shows the config dialog and the user
   cancels (no session created), ReControl_Startup still ran but all commands
   return "no session" errors. ReControl_Shutdown still cleans up correctly.

6. **continue/step are blocking:** These commands don't return until a breakpoint
   is hit (or trace exception fires for step). If no breakpoint is ever hit,
   the connection hangs. Ctrl-C the socat client to disconnect. Use `resume`
   instead of `continue` when you want to let the CPU run without waiting.

7. **Breakpoints are shared with DebugMgr:** ReControl breakpoints use the same
   `MetaMemory::MarkInstructionBreak` infrastructure as POSE's built-in debugger
   (DebugMgr). If both are active, breakpoint hits call both handlers. In practice
   this is fine — DebugMgr's handler only acts when PalmDebugger has set up its
   own breakpoints.

8. **Step uses DOTRACE shortcut:** The step command sets `SPCFLAG_DOTRACE`
   directly instead of going through the normal TRACE→DOTRACE 2-phase mechanism.
   This gives true single-step (one instruction) behavior. The trace exception
   hook is one-shot and cleans up after itself.

9. **Exception handling in install/launch:** The `install` and `launch`
   commands call Palm OS ROM functions via `EmSubroutineCPU68K`. If the ROM
   detects an error during a nested call (e.g. `DmFindDatabase` while inside
   `EmFileImport`), POSE throws `EmExceptionReset`. These commands catch all
   exceptions and return `ERR install/launch failed: emulator exception during
   ROM call` instead of crashing the socket thread. The emulator session may
   be in an unstable state after such an error — use `reset` to recover.

10. **Recovery from stuck states:** If the emulator gets stuck (e.g. `state`
    returns `suspended` with non-zero counters, or `blocked_on_ui` from an
    error dialog), the recommended recovery sequence is:
    ```
    state              # diagnose the problem
    resume             # force-clear all suspend counters
    reset              # schedule a soft reset if needed
    ```
    The `resume` command uses `ForceResume()` which zeroes all suspend
    counters unconditionally, recovering from any race condition or leaked
    counter state.

---

## Future Phases (not yet implemented)

- **Session save/restore:** `save <filepath>` and `load <filepath>` for `.psf`
  session files.

- **Headless mode:** Run POSE without the FLTK window for fully automated testing.

- **Per-trap argument decoding:** Extend tracing to decode trap arguments by
  prototype (e.g. print `DmOpenDatabase(cardNo=0, dbID=0x3a420, mode=0x1)`)
  instead of raw stack words.
