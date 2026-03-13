# ReControl Refactoring: Dispatch Table + File Split

## Problem

ReControl.cpp is a 3200-line monolith containing all 36 command handlers, dispatch logic, I/O, and worker thread integration. It has:

- **Threading inconsistencies**: `launch` runs `EmSessionStopper` on the main thread (deadlock risk). `watch`/`spy`/`gremlin` status read shared globals on the main thread without guards (race conditions). `dialog` reads CPU registers directly without verification.
- **Dual error formats**: `QueueWorkResult` handlers return raw `"ERR ...\n"` strings. Direct commands use `SendErr()`. Exception handling differs between the two paths.
- **No structural enforcement**: Whether a command uses the worker thread, what stop method it needs, and how errors are reported are all ad-hoc decisions buried in each handler's implementation.
- **Missing validation**: `CmdLoad` doesn't null-check `gDocument`. Various inputs lack bounds checking.

## Design

### Command Categories

Every command declares its threading requirement in a static dispatch table:

| Category | CPU State Required | Runs On | Examples |
|----------|-------------------|---------|---------|
| `kCmdImmediate` | None | Main thread, synchronous | `state`, `quit`, `sleep`, `log list` |
| `kCmdWorkerDirect` | `kStopNow` | Worker thread, fire-and-forget (always OK) | `tap`, `pen`, `key`, `button`, `type` |
| `kCmdWorkerCycle` | `kStopOnCycle` | Worker thread, returns result | `screenshot`, `screen-hash`, `save` |
| `kCmdWorkerSysCall` | `kStopOnSysCall` with timeout | Worker thread, returns result | `launch`, `install`, `info`, `apps`, `delete` |
| `kCmdAdaptive` | Direct if `blocked_on_ui`, else `kStopOnCycle` | Depends on CPU state | `peek`, `regs`, `backtrace` |
| `kCmdCustom` | Handler manages its own threading | Varies | `sleep`, `run`, `dialog` |

### Handler Signature

```cpp
using CmdHandler = std::string (*)(const QStringList& args);

enum CommandCategory {
    kCmdImmediate,
    kCmdWorkerDirect,
    kCmdWorkerCycle,
    kCmdWorkerSysCall,
    kCmdAdaptive,
    kCmdCustom
};

struct CommandEntry {
    const char*     name;
    CommandCategory category;
    int             timeoutMs;   // for kCmdWorkerSysCall (0 = default 5000ms)
    CmdHandler      handler;
};
```

Handlers are **free functions** (not `ReControlSession` member methods). They receive args and return a result string (`"OK\n"`, `"OK data\n"`, `"ERR category: msg\n"`, or multi-line `"OK ...\n.\n"` blocks). They never call `Send()`, `SendErr()`, `QueueWork()`, or `QueueWorkResult()` themselves.

### Dispatch Loop

The dispatch loop in `ReControl.cpp` owns all I/O and threading:

1. Look up command name in the static table.
2. Switch on category:
   - `kCmdImmediate`: call handler inline, `Send()` the result.
   - `kCmdWorkerDirect`: wrap in `QueueWork` with `kStopNow` stopper. Handler runs, always sends `"OK\n"`.
   - `kCmdWorkerCycle`: wrap in `QueueWorkResult` with `kStopOnCycle` stopper. Send result.
   - `kCmdWorkerSysCall`: wrap in `QueueWorkResult` with `kStopOnSysCall` + timeout stopper. Send result.
   - `kCmdAdaptive`: check `gSession->GetState()`; if `kBlockedOnUI`, call handler directly; else use `kStopOnCycle` worker path.
   - `kCmdCustom`: pass a session context object with `Send`/`SendErr` access. Handler manages its own control flow.

The stopper creation, timeout handling, exception wrapping, and `Send()` call all live in the dispatch loop. This makes threading bugs structurally impossible for non-custom commands.

### kCmdCustom Escape Hatch

Three commands need special control flow that doesn't fit the table-driven model:

- **`sleep`**: Pauses command processing with `QTimer::singleShot`, resumes later. Needs direct access to `fProcessingPaused`.
- **`run`**: Recursive dispatch — parses sub-commands and calls `DispatchCommand` in a loop. Needs session context.
- **`dialog`**: Reads pending dialog state and optionally calls `EmDlgQt_RespondToDialog`. The "respond" sub-command has a side effect that doesn't fit the return-a-string model.

These handlers receive a `ReControlSession*` (or a context struct) instead of just returning a string. They are explicitly few and marked.

### File Split

| File | Contents |
|------|----------|
| **ReControl.h** | Public API (`ReControl_Startup`, `ReControl_Shutdown`), `CommandCategory` enum, `CommandEntry` struct, `CmdHandler` typedef, `CustomCmdHandler` typedef |
| **ReControl.cpp** | `ReControlServer`, `ReControlSession` class, dispatch table, dispatch loop, `OnReadyRead`, `Send`/`SendErr`, `QueueWork`/`QueueWorkResult`, `ProcessBufferedCommands` |
| **ReControlCmds_Session.cpp** | `launch`, `install`, `export`, `save`, `load`, `reset`, `quit`, `sleep`*, `apps`, `info`, `state`, `dialog`* |
| **ReControlCmds_Input.cpp** | `tap`, `tap-id`, `pen`, `key`, `type`, `button`, `menu`, `run`* |
| **ReControlCmds_Query.cpp** | `screenshot`, `screen-hash`, `ui`, `peek`, `poke`, `regs`, `backtrace` |
| **ReControlCmds_Debug.cpp** | `break`, `watch`, `spy`, `log`, `gremlin`, `check`, `errorhandling` |
| **ReControlCmds_Profile.cpp** | `profile` (all sub-commands), guarded by `#if HAS_PROFILING` |

Commands marked with * are `kCmdCustom` and use the custom handler signature.

### Error Handling

All non-custom handlers return error strings in the format `"ERR <category>: <message>\n"`. The dispatch loop sends them as-is. Categories:

- `usage` — bad arguments, unknown sub-commands
- `transient` — no session, CPU not available
- `timeout` — `kStopOnSysCall` didn't reach boundary in time
- `fatal` — exception during ROM call, unrecoverable

The dispatch loop adds its own error wrapping:
- If the handler throws, catch and return `"ERR internal: <what>\n"`.
- If `gSession` is null for categories that need it, return `"ERR transient: no session\n"` before calling the handler.
- If `gCPUWorker` is null for worker categories, return `"ERR transient: no CPU worker\n"`.
- If `EmSessionStopper` fails, return the appropriate timeout/transient error.

### Bugs Fixed by This Refactoring

1. **`launch` main-thread deadlock**: Moves to `kCmdWorkerSysCall`, runs on worker thread like all other ROM-calling commands.
2. **`watch`/`spy` status race**: Moves to `kCmdAdaptive` or `kCmdWorkerCycle`, reads under a stopper.
3. **`dialog` register access without guard**: The register-dump portion moves to `kCmdAdaptive` so it runs under a stopper when the CPU isn't already blocked.
4. **`gremlin status` race**: Moves to consistent category.
5. **`CmdLoad` null check**: Handler validates `gDocument` before use.
6. **Dual error format**: All handlers return strings; dispatch loop sends them uniformly.

### What Does NOT Change

- `CPUWorkerThread` class and its `Command` struct — unchanged.
- `ReControlServer` single-connection policy — unchanged.
- The TCP protocol (command names, response formats) — unchanged. Clients see no difference.
- `OnReadyRead`, `OnDisconnected`, `OnSleepDone` slot mechanics — unchanged.
- `ProcessBufferedCommands` — unchanged.

## Testing

- All 36 commands must produce identical output before and after the refactor.
- The existing `test_recontrol_stress.py` script covers basic command execution.
- Manual testing via `nc` for launch (the command that changed threading model).
- Verify no deadlocks under rapid command sequences.
