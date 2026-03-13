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

| Category | CPU State Required | Runs On | Stopper |
|----------|-------------------|---------|---------|
| `kCmdImmediate` | None | Main thread, synchronous | None |
| `kCmdWorkerDirect` | None | Worker thread, fire-and-forget, sends OK | None (events use queues with locks) |
| `kCmdWorkerCycle` | Stopped at cycle boundary | Worker thread, returns result | `kStopOnCycle` |
| `kCmdWorkerSysCall` | Stopped at syscall boundary | Worker thread, returns result | `kStopOnSysCall` + timeout |
| `kCmdWorkerRaw` | Handler decides | Worker thread, returns result | None (handler creates own stopper) |
| `kCmdAdaptive` | Direct if `blocked_on_ui`, else worker | Main or worker | `kStopOnCycle` (when not blocked) |
| `kCmdCustom` | Handler manages own threading | Varies | Handler's responsibility |

### Complete Command-to-Category Table

| # | Command | Category | Timeout | File | Notes |
|---|---------|----------|---------|------|-------|
| 1 | `state` | Immediate | — | Session | Reads session state only |
| 2 | `quit` | Immediate | — | Session | Sets app quit flag |
| 3 | `tap` | WorkerDirect | — | Input | Pen down + up |
| 4 | `tap-id` | WorkerCycle | — | Input | Reads form memory, then injects pen |
| 5 | `pen` | WorkerDirect | — | Input | Single pen event |
| 6 | `key` | WorkerDirect | — | Input | Single key event |
| 7 | `type` | WorkerDirect | — | Input | Multi-key sequence |
| 8 | `button` | WorkerDirect | — | Input | Skin button press |
| 9 | `reset` | Custom | — | Session | ForceReset + dialog dismiss; unique control flow |
| 10 | `screenshot` | WorkerRaw | — | Query | Uses kStopNow internally |
| 11 | `screen-hash` | WorkerRaw | — | Query | Uses kStopNow internally |
| 12 | `sleep` | Custom | — | Session | Pauses processing via QTimer |
| 13 | `install` | WorkerRaw | — | Session | Dynamic timeout scales with file size |
| 14 | `export` | WorkerSysCall | 5000 | Session | ROM calls for DB export |
| 15 | `launch` | WorkerSysCall | 5000 | Session | SetSwitchApp + EvtWakeup |
| 16 | `save` | WorkerRaw | — | Session | Uses kStopNow internally |
| 17 | `load` | Custom | — | Session | Tears down/rebuilds session, deferred execution |
| 18 | `info` | WorkerRaw | — | Session | Uses kStopNow; null-session returns version only |
| 19 | `ui` | WorkerCycle | — | Query | Reads form structure |
| 20 | `apps` | WorkerSysCall | 5000 | Session | ROM calls (DmGetNextDatabaseByTypeCreator) |
| 21 | `dialog` | Custom | — | Session | Reads Qt dialog state, optionally responds |
| 22 | `run` | Custom | — | Input | Recursive dispatch, repeat/brace parsing |
| 23 | `peek` | Adaptive | — | Query | Direct if blocked_on_ui |
| 24 | `poke` | Adaptive | — | Query | Changed from WorkerCycle to match peek |
| 25 | `regs` | Adaptive | — | Query | Direct if blocked_on_ui |
| 26 | `menu` | Custom | — | Input | Two-phase async retry with QTimer |
| 27 | `delete` | WorkerSysCall | 5000 | Session | ROM call to delete DB |
| 28 | `backtrace` | Adaptive | — | Query | Direct if blocked_on_ui |
| 29 | `bt` | (alias) | — | — | Alias for `backtrace` in dispatch table |
| 30 | `break` | WorkerCycle | — | Debug | All sub-commands run under stopper (fixes race in `list`) |
| 31 | `watch` | WorkerCycle | — | Debug | All sub-commands under stopper (fixes `status` race) |
| 32 | `spy` | WorkerCycle | — | Debug | All sub-commands under stopper (fixes `status` race) |
| 33 | `log` | Immediate | — | Debug | Preference reads/writes only |
| 34 | `gremlin` | WorkerRaw | — | Debug | Mixed stoppers per sub-command (new=kStopOnSysCall, stop=kStopNow, etc.) |
| 35 | `check` | Immediate | — | Debug | Preference reads/writes only |
| 36 | `errorhandling` | Immediate | — | Debug | Preference reads/writes only |
| 37 | `profile` | WorkerRaw | — | Profile | `#if HAS_PROFILING`; mixed stoppers per sub-command |

**Summary**: 6 Immediate, 5 WorkerDirect, 4 WorkerCycle, 4 WorkerSysCall, 7 WorkerRaw, 4 Adaptive, 6 Custom, 1 alias = 37 table entries.

### Sub-command threading unification

Several commands (`break`, `watch`, `spy`, `gremlin`) currently have sub-commands with different threading models (e.g., `break list` on main thread, `break set` on worker). The refactoring **unifies all sub-commands under the parent's strictest required category** (`kCmdWorkerCycle`). This:

- Fixes race conditions where `status`/`list` sub-commands read shared globals without a stopper
- Simplifies the handler: one function, one threading model, sub-command dispatch is just string matching inside the handler
- Minor cost: `break list` now briefly stops the CPU. This is negligible since the stopper is near-instant for `kStopOnCycle`.

### Handler Signatures

```cpp
// Standard handler: receives args, returns result string.
// Dispatch loop handles threading, stopper, Send().
using CmdHandler = std::string (*)(const QStringList& args);

// Custom handler: receives session context for direct I/O control.
// Handler manages its own threading, Send/SendErr calls.
class ReControlSession;  // forward
using CustomCmdHandler = void (*)(ReControlSession* session, const QStringList& args);

enum CommandCategory {
    kCmdImmediate,
    kCmdWorkerDirect,
    kCmdWorkerCycle,
    kCmdWorkerSysCall,
    kCmdWorkerRaw,
    kCmdAdaptive,
    kCmdCustom
};

struct CommandEntry {
    const char*     name;
    CommandCategory category;
    int             timeoutMs;     // for kCmdWorkerSysCall (0 = default 5000ms)
    CmdHandler      handler;       // for non-Custom categories
    CustomCmdHandler customHandler; // for kCmdCustom (handler is null)
};
```

Handlers are **free functions** in their respective `ReControlCmds_*.cpp` files. Standard handlers never call `Send()`, `SendErr()`, `QueueWork()`, or `QueueWorkResult()`.

Custom handlers receive `ReControlSession*` and call `Send()`/`SendErr()` directly. The session class exposes these as public methods for custom handlers.

### Dispatch Loop

The dispatch loop in `ReControl.cpp` owns all I/O and threading:

1. Look up command name in the static table (linear scan or `std::unordered_map`).
2. Switch on category:
   - `kCmdImmediate`: call handler inline, `Send()` the result.
   - `kCmdWorkerDirect`: wrap handler in `QueueWork`. Always sends `"OK\n"` on completion. Exceptions are swallowed (existing behavior, documented).
   - `kCmdWorkerCycle`: wrap in `QueueWorkResult`. Lambda creates `EmSessionStopper(gSession, kStopOnCycle)`, checks `Stopped()`, calls handler, sends result.
   - `kCmdWorkerSysCall`: wrap in `QueueWorkResult`. Lambda creates `EmSessionStopper(gSession, kStopOnSysCall, timeoutMs)`, checks `Stopped()` and `CanCall()`, calls handler, sends result.
   - `kCmdAdaptive`: on main thread, check `gSession->GetSessionState()`. If `kBlockedOnUI`, call handler directly and send result. Otherwise, use `kCmdWorkerCycle` path.
   - `kCmdCustom`: call `customHandler(this, parts)`. Handler does everything.
3. Pre-handler validation (for non-Immediate, non-Custom):
   - If `gSession` is null → `"ERR transient: no session\n"`
   - If `gCPUWorker` is null → `"ERR transient: no CPU worker\n"`
   - If stopper fails → `"ERR timeout: ..."` or `"ERR transient: could not stop session\n"`
   - If handler throws → `"ERR internal: <what>\n"`

### File Split

| File | Contents |
|------|----------|
| **ReControl.h** | Public API (`ReControl_Startup`, `ReControl_Shutdown`), `CommandCategory` enum, `CommandEntry` struct, handler typedefs, `ReControlSession` public interface (Send/SendErr for custom handlers) |
| **ReControl.cpp** | `ReControlServer`, `ReControlSession` class, dispatch table, dispatch loop, `OnReadyRead`, `Send`/`SendErr`, `QueueWork`/`QueueWorkResult`, `ProcessBufferedCommands` |
| **ReControlCmds_Session.cpp** | `state`, `quit`, `reset`*, `sleep`*, `install`, `export`, `launch`, `save`, `load`*, `info`, `apps`, `dialog`*, `delete` |
| **ReControlCmds_Input.cpp** | `tap`, `tap-id`, `pen`, `key`, `type`, `button`, `menu`*, `run`* |
| **ReControlCmds_Query.cpp** | `screenshot`, `screen-hash`, `ui`, `peek`, `poke`, `regs`, `backtrace` |
| **ReControlCmds_Debug.cpp** | `break`, `watch`, `spy`, `log`, `gremlin`, `check`, `errorhandling` |
| **ReControlCmds_Profile.cpp** | `profile` (all sub-commands), guarded by `#if HAS_PROFILING` |

Commands marked with * are `kCmdCustom`.

### Error Handling

All standard handlers return error strings in the format `"ERR <category>: <message>\n"`. The dispatch loop sends them as-is. Categories:

- `usage` — bad arguments, unknown sub-commands
- `transient` — no session, CPU not available
- `timeout` — `kStopOnSysCall` didn't reach boundary in time
- `fatal` — exception during ROM call, unrecoverable

The dispatch loop adds its own wrapping:
- If handler throws → `"ERR internal: <what>\n"`
- Pre-handler null checks for `gSession` and `gCPUWorker`
- Stopper failure → appropriate timeout/transient error

`kCmdWorkerDirect` always sends `"OK\n"`. If the handler throws, the exception is swallowed to prevent worker thread death. This is existing behavior and is documented as a known limitation.

### Bugs Fixed by This Refactoring

1. **`launch` main-thread deadlock**: Moves to `kCmdWorkerSysCall`.
2. **`watch`/`spy`/`gremlin` status race**: Sub-commands unified under `kCmdWorkerCycle`, all run under stopper.
3. **`break list` race**: Now runs under stopper like `break set`.
4. **`poke` unusable during dialog**: Changed to `kCmdAdaptive` (matches `peek`).
5. **`CmdLoad` null check**: Handler validates `gDocument` before use.
6. **Dual error format**: Standard handlers return strings; dispatch loop sends uniformly.

### What Does NOT Change

- `CPUWorkerThread` class and its `Command` struct.
- `ReControlServer` single-connection policy.
- The TCP protocol (command names, response formats). Clients see no difference.
- `OnReadyRead`, `OnDisconnected`, `OnSleepDone` slot mechanics.
- `ProcessBufferedCommands`.
- The `bt` alias for `backtrace` (preserved as a separate table entry).

## Testing

- All 36 commands must produce identical output before and after the refactor.
- The existing `test_recontrol_stress.py` script covers basic command execution.
- Manual testing via `nc` for `launch` (changed threading model).
- Verify no deadlocks under rapid command sequences.
- Test `break list`, `watch status`, `spy status`, `gremlin status` under load to confirm race fixes.
- Test `poke` while a dialog is pending (new `kCmdAdaptive` behavior).
