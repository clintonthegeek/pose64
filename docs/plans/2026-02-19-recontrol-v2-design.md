# ReControl v2: IPC API for POSE64

## Status: Design approved 2026-02-19, updated 2026-02-20

## Overview

ReControl v2 is a TCP-based IPC API for POSE64, providing programmatic
remote control of the emulator for AI-driven interaction and automated
test scripts.  It replaces the unreleased ReControl v1 (built for
RePOSE4/POSE 3.5) which suffered from critical threading bugs
documented in `docs/ReControlPostMortem/`.

## Context and Lessons Learned

ReControl v1 ran a dedicated pthread that listened on a Unix domain
socket and called `EmSessionStopper` directly to pause the CPU.  This
created a race condition with the FLTK UI thread, which also called
`SuspendThread` for screen repaints.  Three critical bugs resulted:

1. **Irrecoverable suspension** — exceptions during `ExecuteSubroutine`
   left orphaned suspend counters that no resume mechanism could clear
2. **Intermittent bus errors on install** — likely heap corruption from
   race conditions during nested ROM calls
3. **"Could not stop session" errors** — `EmSessionStopper` failures
   from competing `SuspendThread` callers

The root cause in all three cases: two threads competing for
`SuspendThread`.  ReControl v2 eliminates this by design.

## Architecture

### Qt Event Loop + CPUWorkerThread

All socket I/O and command dispatch runs on the **UI thread** via Qt's
signal/slot mechanism.  There is no separate socket thread.

Command handlers that interact with the CPU run on a dedicated
**CPUWorkerThread** to avoid blocking the Qt event loop.  This
includes _all_ commands that call `EmSessionStopper` or
`PostPenEvent`/`PostKeyEvent` (which internally call `PrvWakeUpCPU`,
blocking until the CPU reaches a syscall boundary).

```
  TCP Client -------> Qt Event Loop (UI Thread)
  (socat/script/       |
   Claude)        QTcpServer -> ReControlServer
                       |
                  readyRead -> ReControlSession
                       |
                  Parse command, build Command struct
                       |
                       v
               CPUWorkerThread::queueCommand()
                       |
          +------------+------------+
          |   CPUWorkerThread       |
          |   (dedicated QThread)   |
          |                         |
          |   handler() runs here:  |
          |     PostPenEvent()      |
          |     PostKeyEvent()      |
          |     EmSessionStopper    |
          |     etc.                |
          |                         |
          |   response() delivered  |
          |   to main thread via    |
          |   QMetaObject::invoke   |
          +------------+------------+
                       |
          +------------+------------+
          |   CPU Thread (omni_thread) |
          |   EmSession::Run()         |
          |   EmCPU68K::Execute()      |
          +----------------------------+
```

> **2026-02-20 correction:** The original design assumed
> `PostPenEvent`/`PostKeyEvent` were non-blocking "thread-safe" calls.
> In fact, they internally call `PrvWakeUpCPU`, which creates an
> `EmSessionStopper(kStopOnSysCall)` and blocks the caller until the
> CPU reaches a syscall boundary.  Calling these from the UI thread
> froze the event loop.  The CPUWorkerThread moves all blocking work
> off the main thread.

**Why this works:** Only the CPUWorkerThread ever calls
`SuspendThread` (via `EmSessionStopper` or `PrvWakeUpCPU`).  The
main thread stays free to process Qt events, socket I/O, and
response callbacks.  Response lambdas are delivered back to the main
thread via `QMetaObject::invokeMethod(QCoreApplication::instance(),
lambda, Qt::QueuedConnection)`.

### Components

- **ReControlServer** (QObject) — owns a `QTcpServer`, listens on a
  configurable TCP port, manages connection lifecycle
- **ReControlSession** (QObject) — wraps a `QTcpSocket` for one active
  client connection, dispatches commands, manages sleep state
- **CPUWorkerThread** (QThread) — dedicated thread for executing
  command handlers that block on EmSessionStopper or PrvWakeUpCPU.
  Receives commands via thread-safe queue, delivers response callbacks
  to the main thread via `QMetaObject::invokeMethod`
- **PalmFormReader** — reads PalmOS form structures directly from
  emulated memory for the `ui` command

## Transport

**Protocol:** TCP on `localhost:6416` (configurable via `--port` CLI
flag).  Port 6416 chosen because it's unregistered with IANA and "64"
references POSE64.

**Why TCP instead of Unix sockets:** POSE64 runs on both Linux and
Windows.  TCP works on both platforms out of the box.

**Framing:** Line-based text.  Each command is a single line terminated
by `\n`.  Each response is a single line terminated by `\n`, or
multi-line terminated by `.\n`.

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
ERR <category>: <message>\n
```

**Error categories:**
- `transient` — retry may work (e.g., session temporarily busy)
- `timeout` — CPU did not reach required state in time
- `fatal` — session may be corrupted (recommend reset)
- `usage` — client mistake (bad command, missing args)

**Connection model:** One active client at a time.  Additional
connections are rejected with `ERR busy\n` and immediately closed.

## Phase 1 Command Set

All commands that interact with the CPU run their handlers on the
CPUWorkerThread.  Only `quit` and `sleep` execute entirely on the
main thread.

### Main-thread-only commands

| Command | Description |
|---------|-------------|
| `quit` | Exit POSE64 |
| `sleep <ms>` | Pause command processing (max 30000ms) |

### Worker-thread commands (event injection)

These commands post events to thread-safe queues.  They still run on
the worker thread because `PostPenEvent`/`PostKeyEvent` internally
call `PrvWakeUpCPU`, which blocks via `EmSessionStopper(kStopOnSysCall,
2000)`.  `button` uses atomics and does not block, but runs on the
worker thread for consistency.

| Command | Mechanism | Description |
|---------|-----------|-------------|
| `tap <x> <y>` | PostPenEvent (blocks via PrvWakeUpCPU) | Pen down+up at LCD coordinates |
| `pen <down\|up> <x> <y>` | PostPenEvent (blocks via PrvWakeUpCPU) | Raw pen event (for drags) |
| `key <charcode>` | PostKeyEvent (blocks via PrvWakeUpCPU) | Enqueue key event (decimal charcode) |
| `button <name> <down\|up\|tap>` | atomic fetch_or (non-blocking) | Hardware button press/release/tap |

### Worker-thread commands (CPU-state access)

These commands explicitly stop the CPU with `EmSessionStopper` to
read or modify emulator state.

| Command | Stop Method | Description |
|---------|------------|-------------|
| `state` | kStopOnCycle | Session state + suspend reason + diagnostic counters |
| `info` | kStopNow | POSE64 instance details (device, ROM, serial, screen, session) |
| `screenshot <filepath>` | kStopNow | Capture LCD as PNG via QImage |
| `ui` | kStopOnCycle | Dump form structure via direct memory reads |
| `install <filepath>` | kStopOnSysCall (5s timeout) | Load PRC/PDB into device |
| `launch <dbname>` | kStopOnSysCall (5s timeout) | Launch app by database name |
| `save <filepath>` | kStopNow | Save session to .psf file |
| `load <filepath>` | kStopNow | Load session from .psf file (not yet implemented) |
| `reset [soft\|hard\|debug]` | kStopNow | Schedule device reset (default: soft) |

### Phase 2 commands (future)

`peek`, `poke`, `regs`, `break`, `delbreak`, `breaks`, `step`,
`continue`, `trace`, `resume`.

## The `ui` Command: Direct Memory Reading

### Approach

The old ReControl called PalmOS ROM functions (`FrmGetActiveForm`,
`FrmGetObjectType`, `CtlGetLabel`, etc.) via ROMStubs and
`ExecuteSubroutine`.  This was the code path that caused the worst
bugs — exceptions during nested ROM calls left orphaned suspend
counters and corrupted heap state.

ReControl v2 reads PalmOS form structures **directly from emulated
memory** using `EmMemGet8/16/32` with `CEnableFullAccess`.  The CPU is
stopped with `kStopOnCycle` (safe, no syscall boundary needed), and
the form data is read by following pointers through the struct chain.

### Why this works

PalmOS form structures are defined in the SDK headers shipped with
POSE64 (`src/core/Palm/Platform/Incs/Core/UI/Form.h`, `Control.h`,
`Field.h`, `List.h`, etc.).  These struct layouts are ABI-stable
across PalmOS 2.0-4.1 — they're part of the public API.

### Reading sequence

1. Stop CPU with `EmSessionStopper(kStopOnCycle)`
2. Enable full memory access with `CEnableFullAccess`
3. Read `UICurrentFrmP` (active form pointer) from PalmOS low-memory
   globals
4. Follow `FormType` struct: read `numObjects`, follow `objects`
   pointer to `FormObjListType` array
5. For each object: read `objectType` and object pointer, then read
   the type-specific struct (`ControlType`, `FieldType`, `ListType`,
   etc.)
6. For string fields: follow `char*` pointers and read bytes until
   null terminator (capped at 256 bytes)

### Struct offset handling

The PalmOS structs use m68k layout (2-byte alignment, 4-byte
pointers, big-endian).  Host-side `offsetof()` gives wrong answers
because the host compiler uses different pointer sizes and alignment.

**Solution:** Hardcode m68k struct offsets as constants in
`PalmFormReader.h`, derived from the SDK headers and m68k ABI rules.
Validate these offsets using a calibration PRC test fixture (see
Testing section).

### Safety measures

- **Null pointer checks** on every pointer dereference
- **String length cap** of 256 bytes to prevent runaway reads
- **Memory range validation** — pointers must fall within RAM
  (0x00000000-0x00FFFFFF) or ROM (0x10000000-0x10FFFFFF); invalid
  pointers get `(bad ptr)` annotation
- **`kStopOnCycle` only** — never hangs, unlike `kStopOnSysCall`

### Output format

Multi-line, `.` terminated.  Same format as original ReControl:

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
<-  LABEL id=1006 "Priority:" (10,30)
<- .
```

Focused object prefixed with `*`.  Custom-draw lists show
`(custom-draw)`.  Strings truncated at 80 chars with `...`.

## EmSession Modernization

Three targeted fixes to the EmSession state machine.  The
counter-based suspension design is fundamentally sound — the bugs
were in missing cleanup, not the design itself.

### Fix 1: Exception-safe ExecuteSubroutine

**Problem:** When `CallCPU()` throws inside `ExecuteSubroutine`, the
saved `oldState` is never restored, leaving orphaned suspend counters.

**Fix:** Wrap the execution loop in try/catch that restores
`oldState` before re-throwing:

```cpp
void EmSession::ExecuteSubroutine(void) {
    EmSuspendCounters oldState = fSuspendState.fCounters;
    fSuspendState.fAllCounters = 0;

    try {
        // ... existing loop with CallCPU() ...
    } catch (...) {
        int liveUIThread = fSuspendState.fCounters.fSuspendByUIThread;
        fSuspendState.fCounters = oldState;
        fSuspendState.fCounters.fSuspendByUIThread += liveUIThread;
        throw;
    }

    // ... existing restore logic ...
}
```

`fNestLevel` is already protected by `EmValueChanger` (RAII).

### Fix 2: Timed SuspendThread(kStopOnSysCall)

**Problem:** `SuspendThread(kStopOnSysCall)` waits forever if the CPU
never reaches a syscall.

**Fix:** Add a timeout parameter (default 5 seconds) using the
existing `timedwait()` support on the condition variable.  On timeout,
clean up `fBreakOnSysCall` and return `false`.

> **2026-02-20 status: Partially implemented.** A 2000ms timeout was
> added to `PrvWakeUpCPU` in `EmSession.cpp` — the primary call site
> that caused indefinite blocking.  The general `SuspendThread` API
> already accepts a timeout parameter; this fix ensures the most
> critical caller uses it.  CPU-state commands (`install`, `launch`)
> pass their own timeouts (5000ms) via `EmSessionStopper`.

### Fix 3: Public ForceResume / ResumeFromDebugger

These methods already exist in POSE64 (ported from patched RePOSE4).
Ensure they are declared public on `EmSession` and documented as the
standard recovery mechanisms.

### What stays unchanged

- `EmSuspendCounters` bitfield union
- `EmSessionState` enum
- `SuspendThread`/`ResumeThread` pairing for `fSuspendByUIThread`
- `BlockOnDialog` behavior

## File Organization

### New files

| File | Description | Status |
|------|-------------|--------|
| `src/core/ReControl.h` | Public API: `ReControl_Startup(port)`, `ReControl_Shutdown()` | Done |
| `src/core/ReControl.cpp` | ReControlServer, ReControlSession, command handlers | Done |
| `src/core/PalmFormReader.h` | M68k struct offset constants, memory reading helpers | Done (needs debugging) |
| `src/core/PalmFormReader.cpp` | Form-to-text serializer for `ui` command | Done (returns minimal data) |
| `src/core/CPUWorkerThread.h` | Command queue, thread lifecycle, signal definitions | Done |
| `src/core/CPUWorkerThread.cpp` | Worker thread loop, QMetaObject response delivery | Done |

### Modified files

| File | Change | Status |
|------|--------|--------|
| `src/ui/main.cpp` | Parse `--port`, call `ReControl_Startup`/`Shutdown` | Done |
| `src/core/EmSession.h` | Timeout param on `SuspendThread`, public `ForceResume`/`ResumeFromDebugger` | Partial |
| `src/core/EmSession.cpp` | Timed `kStopOnSysCall` in `PrvWakeUpCPU` (2000ms) | Done |
| `CMakeLists.txt` | Add source files, link `Qt6::Network` | Done |

### Test fixtures

| File | Description |
|------|-------------|
| `tests/calibration/FormCalibration.c` | PalmOS C source for calibration PRC |
| `tests/calibration/verify_offsets.py` | Installs PRC, reads memory, validates offsets |

## Connection Lifecycle

### Startup

```
main() parses --port (default 6416)
  -> ReControl_Startup(port)
    -> Creates ReControlServer (QObject, parent = qApp)
    -> QTcpServer::listen(QHostAddress::LocalHost, port)
    -> Logs "ReControl listening on localhost:6416"
  -> qtApp.exec()
```

If the port is in use, log a warning but don't abort POSE64.

### Connection

```
QTcpServer::newConnection
  -> If active session: reject with "ERR busy\n", close
  -> Else: create ReControlSession(socket)
    -> readyRead -> dispatch commands
    -> disconnected -> delete session, ready for next
```

### Sleep command

```
CmdSleep(ms):
  -> fProcessingPaused = true
  -> QTimer::singleShot(ms, onSleepDone)
  -> Return without sending response

onSleepDone:
  -> Send "OK\n"
  -> fProcessingPaused = false
  -> Process buffered commands

onReadyRead (when paused):
  -> Buffer lines, don't dispatch
```

UI thread stays responsive during sleep.  CPU keeps running.

### Shutdown

```
ReControl_Shutdown():
  -> Close QTcpServer
  -> If active session: send "ERR server shutting down\n", close
  -> Delete ReControlServer
```

## Testing Strategy

### Calibration PRC

A minimal PalmOS application that creates a form with one of each
object type (button, field, list, checkbox, label, scrollbar, etc.)
and writes the addresses of the form and each object to a known
memory location.  Built with `m68k-palmos-gcc`, checked into the repo
as a pre-built `.prc`.

Used as a one-time development validation step to confirm that the
hardcoded m68k struct offsets in `PalmFormReader.h` match reality.
Also serves as a regression test for future `ui` extensions.

### Integration testing

Manual testing with `socat` against a running POSE64 instance,
exercising each command.  The same workflows from the original
ReControl documentation apply:

```bash
socat - TCP:localhost:6416 <<'EOF'
state
install /path/to/app.prc
launch AppName
sleep 2000
screenshot /tmp/test.png
ui
EOF
```

## Remaining Work (as of 2026-02-20)

### Phase 1 completion

1. **Debug PalmFormReader** — `ui` command currently returns minimal
   data (`OK FORM 0 0` followed by `.`).  The form reading logic
   needs investigation; likely incorrect struct offsets or a failure
   to locate `UICurrentFrmP`.

2. **Implement `load` command** — currently a stub returning
   `ERR usage: load not yet implemented`.  Requires EmDocument
   cooperation to replace the running session.

3. **Fix 1: Exception-safe ExecuteSubroutine** — not yet implemented.
   Low priority for ReControl v2 since we no longer call
   `ExecuteSubroutine` from ReControl, but still a correctness fix
   for the EmSession state machine.

4. **Fix 3: Public ForceResume/ResumeFromDebugger** — verify these
   are public and documented.

5. **Integration test pass** — run the full command set against a
   live instance and verify each command works end-to-end.

### Verified working (2026-02-20)

- `tap`, `key`, `button`, `pen` — event injection via worker thread
- `screenshot` — captures LCD as PNG
- `state`, `info` — return session info
- `install`, `launch` — install PRC and launch apps
- `save` — save session to .psf
- `reset` — soft/hard/debug reset
- `sleep`, `quit` — main-thread commands
- Response delivery via `QMetaObject::invokeMethod`
- No main-thread freezes during command processing
