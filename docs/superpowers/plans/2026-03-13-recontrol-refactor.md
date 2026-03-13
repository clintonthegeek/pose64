# ReControl Refactoring Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace ReControl.cpp's ad-hoc command dispatch with a static dispatch table that enforces threading safety, and split the 3200-line monolith into 7 files by command group.

**Architecture:** A static `CommandEntry` table maps command names to categories (`kCmdImmediate`, `kCmdWorkerDirect`, `kCmdWorkerCycle`, `kCmdWorkerSysCall`, `kCmdWorkerRaw`, `kCmdAdaptive`, `kCmdCustom`). The dispatch loop handles stopper creation, worker queuing, error wrapping, and `Send()`. Command handlers are free functions that return result strings — they never touch I/O or threading directly (except `kCmdCustom` handlers). `kCmdWorkerRaw` dispatches to the worker with exception handling but no stopper — for commands that need non-standard stopper types or dynamic timeouts.

**Tech Stack:** C++11, Qt6 (QTcpServer/QTcpSocket/QObject), CMake with GLOB_RECURSE (new files auto-discovered).

**Spec:** `docs/superpowers/specs/2026-03-13-recontrol-refactor-design.md`

---

## Chunk 1: Infrastructure (ReControl.h + dispatch table + dispatch loop)

### Task 1: Expand ReControl.h with types and forward declarations

**Files:**
- Modify: `src/core/ReControl.h`

- [ ] **Step 1: Add the dispatch types to ReControl.h**

Replace the current contents of `src/core/ReControl.h` with:

```cpp
/* -*- mode: C++; tab-width: 4 -*- */
/* ReControl: TCP control interface for POSE64. */

#ifndef ReControl_h
#define ReControl_h

#include <string>
#include <QStringList>

// Forward declarations
class CPUWorkerThread;
class ReControlSession;

// Global instance - initialized in main.cpp
extern CPUWorkerThread* gCPUWorker;

// ---------------------------------------------------------------------------
// Command dispatch types
// ---------------------------------------------------------------------------

enum CommandCategory {
	kCmdImmediate,       // main thread, no stopper
	kCmdWorkerDirect,    // worker thread, no stopper, always sends OK
	kCmdWorkerCycle,     // worker thread, kStopOnCycle, returns result
	kCmdWorkerSysCall,   // worker thread, kStopOnSysCall + timeout
	kCmdWorkerRaw,       // worker thread, no stopper (handler creates own)
	kCmdAdaptive,        // direct if blocked_on_ui, else kStopOnCycle
	kCmdCustom           // handler manages own threading
};

// Standard handler: returns result string ("OK\n", "ERR ...\n", multi-line).
// Never calls Send/SendErr/QueueWork directly.
using CmdHandler = std::string (*)(const QStringList& args);

// Custom handler: receives session for direct I/O control.
using CustomCmdHandler = void (*)(ReControlSession* session,
                                  const QStringList& args);

struct CommandEntry {
	const char*      name;
	CommandCategory  category;
	int              timeoutMs;      // for kCmdWorkerSysCall (0 = default 5000)
	CmdHandler       handler;        // non-null for standard categories
	CustomCmdHandler customHandler;  // non-null for kCmdCustom
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ReControl_Startup (int port);
void ReControl_Shutdown (void);

#endif /* ReControl_h */
```

- [ ] **Step 2: Build to verify header compiles**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -10`
Expected: Compiles successfully (existing code still uses the old header contents; new types are additive).

- [ ] **Step 3: Commit**

```bash
git add src/core/ReControl.h
git commit -m "refactor: expand ReControl.h with dispatch table types"
```

---

### Task 2: Add the dispatch table and new dispatch loop to ReControl.cpp

This is the core change. We add the static command table, rewrite `DispatchCommand` to use it, and expose `Send`/`SendErr` as public methods for custom handlers.

**Files:**
- Modify: `src/core/ReControl.cpp`

**Important context:**
- `ReControlSession` and `ReControlServer` are Q_OBJECT classes defined in `ReControl.cpp` (not the header), with `#include "ReControl.moc"` at the end.
- The `QueueWork` and `QueueWorkResult` methods remain as private helpers — the dispatch loop uses them.
- Command handler functions are declared `extern` (defined in `ReControlCmds_*.cpp` files, created in later tasks). Until those files exist, we use temporary stubs.

- [ ] **Step 1: Move Send/SendErr to public, add DispatchCommand helpers**

In the `ReControlSession` class declaration (currently at line 76), move `Send` and `SendErr` from `private` to `public`:

```cpp
class ReControlSession : public QObject
{
	Q_OBJECT

public:
	ReControlSession (QTcpSocket* socket, ReControlServer* server);
	~ReControlSession ();

	// Public for custom command handlers
	void Send (const std::string& msg);
	void SendErr (const std::string& category, const std::string& msg);

private slots:
	void OnReadyRead ();
	void OnDisconnected ();
	void OnSleepDone ();

private:
	void ProcessBufferedCommands (void);
	void DispatchCommand (const QStringList& parts);

	void QueueWork (std::function<void()> handler);
	void QueueWorkResult (std::function<std::string()> handler);

	QTcpSocket* fSocket;
	ReControlServer* fServer;
	QByteArray fReadBuffer;
	bool fProcessingPaused;
	QStringList fCommandBuffer;
};
```

Note: All `CmdXxx` member functions are removed from the class declaration — they will become free functions / extern declarations.

- [ ] **Step 2: Add the static dispatch table**

After the `ReControlSession` constructor/destructor and the `Send`/`SendErr`/`QueueWork`/`QueueWorkResult` implementations, add the dispatch table. For now, use `nullptr` for all handler pointers — we'll fill them in as we extract commands:

```cpp
// ---------------------------------------------------------------------------
// Command handler forward declarations
// These are defined in ReControlCmds_*.cpp files.
// ---------------------------------------------------------------------------

// Session commands (ReControlCmds_Session.cpp)
std::string RcCmd_State (const QStringList& args);
std::string RcCmd_Quit (const QStringList& args);
void        RcCmd_Reset (ReControlSession* s, const QStringList& args);
void        RcCmd_Sleep (ReControlSession* s, const QStringList& args);
std::string RcCmd_Install (const QStringList& args);
std::string RcCmd_Export (const QStringList& args);
std::string RcCmd_Launch (const QStringList& args);
std::string RcCmd_Save (const QStringList& args);
void        RcCmd_Load (ReControlSession* s, const QStringList& args);
std::string RcCmd_Info (const QStringList& args);
std::string RcCmd_Apps (const QStringList& args);
void        RcCmd_Dialog (ReControlSession* s, const QStringList& args);
std::string RcCmd_Delete (const QStringList& args);

// Input commands (ReControlCmds_Input.cpp)
std::string RcCmd_Tap (const QStringList& args);
std::string RcCmd_TapId (const QStringList& args);
std::string RcCmd_Pen (const QStringList& args);
std::string RcCmd_Key (const QStringList& args);
std::string RcCmd_Type (const QStringList& args);
std::string RcCmd_Button (const QStringList& args);
void        RcCmd_Menu (ReControlSession* s, const QStringList& args);
void        RcCmd_Run (ReControlSession* s, const QStringList& args);

// Query commands (ReControlCmds_Query.cpp)
std::string RcCmd_Screenshot (const QStringList& args);
std::string RcCmd_ScreenHash (const QStringList& args);
std::string RcCmd_UI (const QStringList& args);
std::string RcCmd_Peek (const QStringList& args);
std::string RcCmd_Poke (const QStringList& args);
std::string RcCmd_Regs (const QStringList& args);
std::string RcCmd_Backtrace (const QStringList& args);

// Debug commands (ReControlCmds_Debug.cpp)
std::string RcCmd_Break (const QStringList& args);
std::string RcCmd_Watch (const QStringList& args);
std::string RcCmd_Spy (const QStringList& args);
std::string RcCmd_Log (const QStringList& args);
std::string RcCmd_Gremlin (const QStringList& args);
std::string RcCmd_Check (const QStringList& args);
std::string RcCmd_ErrorHandling (const QStringList& args);

// Profile commands (ReControlCmds_Profile.cpp)
#if HAS_PROFILING
std::string RcCmd_Profile (const QStringList& args);
#endif

// ---------------------------------------------------------------------------
// Dispatch table
// ---------------------------------------------------------------------------
// {name, category, timeoutMs, handler, customHandler}

static const CommandEntry sCommandTable[] = {
	// Session
	{"state",          kCmdImmediate,    0, RcCmd_State,      nullptr},
	{"quit",           kCmdImmediate,    0, RcCmd_Quit,       nullptr},
	{"reset",          kCmdCustom,       0, nullptr,          RcCmd_Reset},
	{"sleep",          kCmdCustom,       0, nullptr,          RcCmd_Sleep},
	{"install",        kCmdWorkerRaw,    0, RcCmd_Install,   nullptr},
	{"export",         kCmdWorkerSysCall, 5000, RcCmd_Export,  nullptr},
	{"launch",         kCmdWorkerSysCall, 5000, RcCmd_Launch,  nullptr},
	{"save",           kCmdWorkerRaw,   0, RcCmd_Save,       nullptr},
	{"load",           kCmdCustom,       0, nullptr,          RcCmd_Load},
	{"info",           kCmdWorkerRaw,   0, RcCmd_Info,       nullptr},
	{"apps",           kCmdWorkerSysCall, 5000, RcCmd_Apps,    nullptr},
	{"dialog",         kCmdCustom,       0, nullptr,          RcCmd_Dialog},
	{"delete",         kCmdWorkerSysCall, 5000, RcCmd_Delete,  nullptr},

	// Input
	{"tap",            kCmdWorkerDirect, 0, RcCmd_Tap,        nullptr},
	{"tap-id",         kCmdWorkerCycle,  0, RcCmd_TapId,      nullptr},
	{"pen",            kCmdWorkerDirect, 0, RcCmd_Pen,        nullptr},
	{"key",            kCmdWorkerDirect, 0, RcCmd_Key,        nullptr},
	{"type",           kCmdWorkerDirect, 0, RcCmd_Type,       nullptr},
	{"button",         kCmdWorkerDirect, 0, RcCmd_Button,     nullptr},
	{"menu",           kCmdCustom,       0, nullptr,          RcCmd_Menu},
	{"run",            kCmdCustom,       0, nullptr,          RcCmd_Run},

	// Query
	{"screenshot",     kCmdWorkerRaw,   0, RcCmd_Screenshot, nullptr},
	{"screen-hash",    kCmdWorkerRaw,   0, RcCmd_ScreenHash, nullptr},
	{"ui",             kCmdWorkerCycle,  0, RcCmd_UI,         nullptr},
	{"peek",           kCmdAdaptive,     0, RcCmd_Peek,       nullptr},
	{"poke",           kCmdAdaptive,     0, RcCmd_Poke,       nullptr},
	{"regs",           kCmdAdaptive,     0, RcCmd_Regs,       nullptr},
	{"backtrace",      kCmdAdaptive,     0, RcCmd_Backtrace,  nullptr},
	{"bt",             kCmdAdaptive,     0, RcCmd_Backtrace,  nullptr},

	// Debug
	{"break",          kCmdWorkerCycle,  0, RcCmd_Break,      nullptr},
	{"watch",          kCmdWorkerCycle,  0, RcCmd_Watch,      nullptr},
	{"spy",            kCmdWorkerCycle,  0, RcCmd_Spy,        nullptr},
	{"log",            kCmdImmediate,    0, RcCmd_Log,        nullptr},
	{"gremlin",        kCmdWorkerRaw,   0, RcCmd_Gremlin,    nullptr},
	{"check",          kCmdImmediate,    0, RcCmd_Check,      nullptr},
	{"errorhandling",  kCmdImmediate,    0, RcCmd_ErrorHandling, nullptr},

	// Profile
#if HAS_PROFILING
	{"profile",        kCmdWorkerRaw,   0, RcCmd_Profile,    nullptr},
#endif

	{nullptr, kCmdImmediate, 0, nullptr, nullptr}  // sentinel
};
```

- [ ] **Step 3: Rewrite DispatchCommand to use the table**

Replace the existing `DispatchCommand` method (the if/else chain) with:

```cpp
void ReControlSession::DispatchCommand (const QStringList& parts)
{
	if (parts.isEmpty ()) return;
	std::string cmdName = parts[0].toLower ().toStdString ();

	// Look up command in table
	const CommandEntry* entry = nullptr;
	for (const CommandEntry* e = sCommandTable; e->name; e++)
	{
		if (cmdName == e->name)
		{
			entry = e;
			break;
		}
	}

	if (!entry)
	{
		SendErr ("usage", "unknown command '" + cmdName + "'");
		return;
	}

	switch (entry->category)
	{
		case kCmdImmediate:
		{
			std::string result = entry->handler (parts);
			Send (result);
			break;
		}

		case kCmdWorkerDirect:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			// QueueWork sends "OK\n" automatically via its response callback.
			QueueWork ([handler, parts]() {
				if (!gSession) return;
				handler (parts);
			});
			break;
		}

		case kCmdWorkerCycle:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			QueueWorkResult ([handler, parts]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				EmSessionStopper stopper (gSession, kStopOnCycle);
				if (!stopper.Stopped ())
					return "ERR transient: could not stop session\n";
				return handler (parts);
			});
			break;
		}

		case kCmdWorkerSysCall:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			int timeout = entry->timeoutMs > 0 ? entry->timeoutMs : 5000;
			QueueWorkResult ([handler, parts, timeout]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				EmSessionStopper stopper (gSession, kStopOnSysCall, timeout);
				if (!stopper.Stopped () || !stopper.CanCall ())
				{
					char msg[256];
					snprintf (msg, sizeof (msg),
						"ERR timeout: CPU did not reach syscall boundary within %dms\n",
						timeout);
					return msg;
				}
				return handler (parts);
			});
			break;
		}

		case kCmdWorkerRaw:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			QueueWorkResult ([handler, parts]() -> std::string {
				if (!gSession) return "ERR transient: no session\n";
				return handler (parts);
			});
			break;
		}

		case kCmdAdaptive:
		{
			if (!gSession) { SendErr ("transient", "no session"); return; }
			auto handler = entry->handler;
			if (gSession->GetSessionState () == kBlockedOnUI)
			{
				// CPU is blocked on dialog — memory is stable, run directly
				std::string result = handler (parts);
				Send (result);
			}
			else
			{
				QueueWorkResult ([handler, parts]() -> std::string {
					if (!gSession) return "ERR transient: no session\n";
					EmSessionStopper stopper (gSession, kStopOnCycle);
					if (!stopper.Stopped ())
						return "ERR transient: could not stop session\n";
					return handler (parts);
				});
			}
			break;
		}

		case kCmdCustom:
		{
			entry->customHandler (this, parts);
			break;
		}
	}
}
```

- [ ] **Step 4: Build to verify (will fail — handler functions not yet defined)**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | grep "undefined reference" | head -5`
Expected: Linker errors for `RcCmd_*` functions. This confirms the table is wired up and the compiler accepted the new dispatch loop.

- [ ] **Step 5: Commit (WIP — won't link yet)**

```bash
git add src/core/ReControl.cpp
git commit -m "refactor(wip): add dispatch table and table-driven DispatchCommand

Linker errors expected until handler functions are extracted to
ReControlCmds_*.cpp files in subsequent commits."
```

---

## Chunk 2: Extract command handlers into separate files

### Task 3: Create ReControlCmds_Session.cpp

Extract session-related command handlers from `ReControl.cpp` into free functions.

**Files:**
- Create: `src/core/ReControlCmds_Session.cpp`
- Modify: `src/core/ReControl.cpp` (remove extracted functions)

**Handler → free function conversion rules:**
1. Remove `ReControlSession::` prefix → rename to `RcCmd_Xxx`
2. For standard handlers: remove `Send()`/`SendErr()` calls → return the string instead
3. For standard handlers: remove `QueueWorkResult`/`QueueWork` wrappers and `EmSessionStopper` creation — the dispatch loop handles these now
4. For standard handlers: remove `if (!gSession)` null checks at the top — the dispatch loop handles these
5. For custom handlers: keep the `ReControlSession*` parameter, keep `Send()`/`SendErr()` calls
6. Commands that need non-standard stoppers (`install` with dynamic timeout, `save`/`info`/`screenshot`/`screen-hash` with `kStopNow`, `gremlin`/`profile` with mixed sub-command stoppers) use `kCmdWorkerRaw`. The dispatch loop queues them to the worker with exception handling but creates NO stopper — the handler creates its own. This is already reflected in the dispatch table (Task 2) and enum (Task 1).

- [ ] **Step 2: Create ReControlCmds_Session.cpp with all session handlers**

Create `src/core/ReControlCmds_Session.cpp`. Each handler is a free function. Standard handlers return `std::string`. Custom handlers take `ReControlSession*`.

The file needs these includes:
```cpp
#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "EmApplication.h"
#include "EmDocument.h"
#include "EmFileImport.h"
#include "EmStreamFile.h"
#include "EmScreen.h"
#include "EmLowMem.h"
#include "EmTypes.h"
#include "ROMStubs.h"
#include "Patches/EmPatchState.h"
#include "CPUWorkerThread.h"
#include "LoadApplication.h"

#include <QApplication>
#include <QFileInfo>
#include <QTimer>
#include <QPointer>
```

Convert each handler following the rules above. Key examples:

**`RcCmd_State`** (Immediate — no stopper, returns string):
```cpp
std::string RcCmd_State (const QStringList& args)
{
    if (!gSession)
        return "OK stopped\n";

    EmSessionState state = gSession->GetSessionState ();
    switch (state)
    {
        case kRunning:   return "OK running\n";
        case kSuspended: { /* ... same logic, return string ... */ }
        case kStopped:   return "OK stopped\n";
        case kBlockedOnUI: return "OK blocked_on_ui\n";
    }
    return "OK unknown\n";
}
```

**`RcCmd_Install`** (WorkerRaw — handler creates its own stopper):
```cpp
std::string RcCmd_Install (const QStringList& args)
{
    // Arg validation (return "ERR usage: ..." on failure)
    // File validation (exists, not empty, not too large)
    // Compute scaled timeout
    EmSessionStopper stopper (gSession, kStopOnSysCall, timeoutMs);
    if (!stopper.Stopped ()) return "ERR timeout: ...\n";
    // ... import logic, return "OK\n" or "ERR ...\n"
}
```

**`RcCmd_Launch`** (WorkerSysCall — stopper created by dispatch loop):
```cpp
std::string RcCmd_Launch (const QStringList& args)
{
    if (args.size () < 2)
        return "ERR usage: launch <dbname>\n";
    // ... resolve name, SetSwitchApp, EvtWakeup ...
    return "OK\n";
}
```

Note: `launch` currently runs `EmSessionStopper` on the main thread. Under the new system, the dispatch loop runs it on the worker thread via `kCmdWorkerSysCall`. This fixes the main-thread deadlock bug.

**`RcCmd_Load`** (Custom — needs session pointer for deferred Send):
```cpp
void RcCmd_Load (ReControlSession* session, const QStringList& args)
{
    // Same deferred QTimer::singleShot logic as current code
    // Uses session->Send() for responses
}
```

**`RcCmd_Sleep`** (Custom — needs session for fProcessingPaused):

`sleep` is special because it accesses `fProcessingPaused` and `OnSleepDone`, which are private members of `ReControlSession`. Options:
- Make `fProcessingPaused` public (ugly)
- Add public `PauseProcessing(int ms)` method to ReControlSession
- Keep `CmdSleep` as an inline method in ReControl.cpp

**Decision:** Add `PauseProcessing(int ms)` to ReControlSession. The custom handler calls it:

```cpp
void RcCmd_Sleep (ReControlSession* session, const QStringList& args)
{
    if (args.size () != 2)
    {
        session->SendErr ("usage", "sleep <ms>");
        return;
    }
    int ms = args[1].toInt ();
    if (ms < 1 || ms > 30000)
    {
        session->SendErr ("usage", "sleep <1-30000>");
        return;
    }
    session->PauseProcessing (ms);
}
```

And in `ReControlSession`:
```cpp
public:
    void PauseProcessing (int ms);
```
Implementation:
```cpp
void ReControlSession::PauseProcessing (int ms)
{
    fProcessingPaused = true;
    QTimer::singleShot (ms, this, &ReControlSession::OnSleepDone);
}
```

- [ ] **Step 3: Remove extracted handlers from ReControl.cpp**

Delete all `CmdState`, `CmdQuit`, `CmdReset`, `CmdSleep`, `CmdInstall`, `CmdExport`, `CmdLaunch`, `CmdSave`, `CmdLoad`, `CmdInfo`, `CmdApps`, `CmdDialog`, `CmdDelete` methods, the `OnSleepDone` response code (keep the method but it now just calls the existing logic), and the `ComputeScreenHash` helper (moves to Query file).

- [ ] **Step 4: Build**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -15`
Expected: Linker errors only for `RcCmd_*` functions in Input/Query/Debug/Profile files (not Session).

- [ ] **Step 5: Commit**

```bash
git add src/core/ReControlCmds_Session.cpp src/core/ReControl.cpp src/core/ReControl.h
git commit -m "refactor: extract session commands to ReControlCmds_Session.cpp"
```

---

### Task 4: Create ReControlCmds_Input.cpp

**Files:**
- Create: `src/core/ReControlCmds_Input.cpp`
- Modify: `src/core/ReControl.cpp` (remove extracted functions)

- [ ] **Step 1: Create ReControlCmds_Input.cpp**

Extract: `tap`, `tap-id`, `pen`, `key`, `type`, `button` (all WorkerDirect or WorkerCycle — return strings, no Send), `menu` and `run` (Custom — take session pointer).

Includes needed:
```cpp
#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "Skins.h"
#include "ROMStubs.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "PalmFormReader.h"
#include "CPUWorkerThread.h"

#include <QTimer>
```

**WorkerDirect handlers** (`tap`, `pen`, `key`, `type`, `button`): The dispatch loop calls these on the worker thread with `kStopNow` and discards the return value. The handler should still do the work (PostPenEvent, PostKeyEvent, etc.) but does NOT need to create a stopper. However, `kCmdWorkerDirect` does create a `kStopNow` stopper implicitly... wait, actually looking at the current code, `QueueWork` handlers DON'T create stoppers at all — they just call `PostPenEvent`/`PostKeyEvent` directly. These methods are thread-safe (they use queues with locks). So `kCmdWorkerDirect` should NOT create a stopper either.

**Revised `kCmdWorkerDirect` dispatch:**
```cpp
case kCmdWorkerDirect:
{
    if (!gSession) { SendErr ("transient", "no session"); return; }
    auto handler = entry->handler;
    QueueWork ([handler, parts]() {
        if (!gSession) return;
        handler (parts);
    });
    break;
}
```
No stopper — the handler does its own event posting (which is lock-protected).

**`run`** is custom because it recursively calls `DispatchCommand`. Add public `void RunSubCommand (const QStringList& parts)` to `ReControlSession` that calls `DispatchCommand`. The `run` custom handler calls it for each sub-command.

**`menu`** is custom because it uses a two-phase async retry with `QTimer::singleShot`. The existing `DoMenuLookup` helper is tightly coupled to `ReControlSession` (captures `this`, calls `Send`, schedules recursive retries). **Strategy:** Keep `DoMenuLookup` as a public method on `ReControlSession`. The free `RcCmd_Menu` function receives `ReControlSession*` and calls `session->DoMenuLookup(...)`. Move only the arg parsing to the free function.

- [ ] **Step 2: Remove extracted handlers from ReControl.cpp**

- [ ] **Step 3: Build and verify**

Expected: Linker errors only for Query/Debug/Profile handlers.

- [ ] **Step 4: Commit**

```bash
git add src/core/ReControlCmds_Input.cpp src/core/ReControl.cpp
git commit -m "refactor: extract input commands to ReControlCmds_Input.cpp"
```

---

### Task 5: Create ReControlCmds_Query.cpp

**Files:**
- Create: `src/core/ReControlCmds_Query.cpp`
- Modify: `src/core/ReControl.cpp` (remove extracted functions)

- [ ] **Step 1: Create ReControlCmds_Query.cpp**

Extract: `screenshot`, `screen-hash`, `ui`, `peek`, `poke`, `regs`, `backtrace`.

These are all standard handlers (WorkerCycle or Adaptive). The dispatch loop creates the stopper; the handler just does its work and returns a string.

For `screenshot`: move the `ComputeScreenHash` helper here too. The handler constructs QFont/QPainter internally. Since `kCmdWorkerRaw` (or `kCmdWorkerCycle` without stopper) is used, the handler still creates its own stopper — wait, screenshot is `kCmdWorkerCycle`, so the dispatch loop creates a `kStopOnCycle` stopper.

Actually, screenshot currently uses `kStopNow` and does significant work (screen capture, PNG rendering). The stopper is fine as `kStopOnCycle`. The handler receives a stopped CPU.

But screenshot currently does QFont construction. In Qt6, QFont can be constructed from any thread. The concern was QFontDatabase, which is also thread-safe in Qt6 for reads. So this is fine.

`peek`/`poke`/`regs`/`backtrace` are Adaptive — the dispatch loop handles the direct-vs-worker branching. The handler just does its work assuming the CPU is stopped.

Note: `peek` and `poke` have their own `ParseAddress` helper. Move it to this file.

Includes:
```cpp
#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "EmScreen.h"
#include "EmLowMem.h"
#include "Hardware/EmMemory.h"
#include "PalmFormReader.h"
#include "UAE.h"

#include <QImage>
#include <QPainter>
#include <QFont>
#include <zlib.h>
```

- [ ] **Step 2: Remove extracted handlers from ReControl.cpp**

- [ ] **Step 3: Build and verify**

Expected: Linker errors only for Debug/Profile handlers.

- [ ] **Step 4: Commit**

```bash
git add src/core/ReControlCmds_Query.cpp src/core/ReControl.cpp
git commit -m "refactor: extract query commands to ReControlCmds_Query.cpp"
```

---

### Task 6: Create ReControlCmds_Debug.cpp

**Files:**
- Create: `src/core/ReControlCmds_Debug.cpp`
- Modify: `src/core/ReControl.cpp` (remove extracted functions)

- [ ] **Step 1: Create ReControlCmds_Debug.cpp**

Extract: `break`, `watch`, `spy`, `log`, `gremlin`, `check`, `errorhandling`.

`break`/`watch`/`spy`/`gremlin` are `kCmdWorkerCycle` — all sub-commands now run under a stopper (dispatch loop creates it). This fixes the race conditions in `list`/`status` sub-commands.

**Important change for `gremlin`:** Currently `gremlin suspend/step/resume` use `QueueWork` (fire-and-forget). Under the new system, `gremlin` is a single `kCmdWorkerCycle` handler. The handler must return a string for all sub-commands. For `suspend`/`step`/`resume`, just return `"OK\n"` after doing the work.

`log`/`check`/`errorhandling` are `kCmdImmediate` — no stopper needed, run on main thread.

Includes:
```cpp
#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "DebugMgr.h"
#include "Hordes.h"
#include "CGremlins.h"
#include "Logging.h"
#include "PreferenceMgr.h"
#include "ErrorHandling.h"
#include "MetaMemory.h"
#include "Hardware/EmMemory.h"
```

- [ ] **Step 2: Remove extracted handlers from ReControl.cpp**

- [ ] **Step 3: Build and verify**

Expected: Linker errors only for Profile handler (if HAS_PROFILING).

- [ ] **Step 4: Commit**

```bash
git add src/core/ReControlCmds_Debug.cpp src/core/ReControl.cpp
git commit -m "refactor: extract debug commands to ReControlCmds_Debug.cpp"
```

---

### Task 7: Create ReControlCmds_Profile.cpp

**Files:**
- Create: `src/core/ReControlCmds_Profile.cpp`
- Modify: `src/core/ReControl.cpp` (remove extracted function)

- [ ] **Step 1: Create ReControlCmds_Profile.cpp**

Extract `profile` (all sub-commands). Entire file is `#if HAS_PROFILING` guarded.

The handler is `kCmdWorkerCycle` — dispatch loop creates stopper. But some sub-commands (`dump`, `print`) use `kStopNow` and need different validation. **Decision:** Change `profile` to `kCmdWorkerRaw` — handler creates its own stopper per sub-command.

Includes:
```cpp
#include "EmCommon.h"
#include "ReControl.h"
#include "EmSession.h"
#include "Profiling.h"
```

- [ ] **Step 2: Remove extracted handler from ReControl.cpp**

- [ ] **Step 3: Build and verify all link errors resolved**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -15`
Expected: Clean compile with only pre-existing warnings.

- [ ] **Step 4: Commit**

```bash
git add src/core/ReControlCmds_Profile.cpp src/core/ReControl.cpp
git commit -m "refactor: extract profile commands to ReControlCmds_Profile.cpp"
```

---

## Chunk 3: Verification and cleanup

### Task 8: Verify all commands work identically

**Files:** None (testing only)

- [ ] **Step 1: Build clean**

```bash
cd build && cmake --build . -j$(nproc) 2>&1 | tail -5
```
Expected: `[100%] Built target pose64`

- [ ] **Step 2: Launch emulator and test basic commands via nc**

```bash
DISPLAY=:1 WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=/run/user/1000 \
  ./build/pose64 -psf build/freshm515.psf &
sleep 3
# Test immediate commands
echo "state" | nc -q1 localhost 6416
echo "info" | nc -q2 localhost 6416
# Test worker commands
(echo "screenshot /tmp/test_refactor.png"; sleep 2) | nc -q1 localhost 6416
(echo "apps"; sleep 2) | nc -q1 localhost 6416
# Test adaptive commands
(echo "regs"; sleep 2) | nc -q1 localhost 6416
(echo "peek 0 16"; sleep 2) | nc -q1 localhost 6416
# Test launch (the command that changed threading)
(echo "launch Calculator"; sleep 3) | nc -q1 localhost 6416
(echo "screenshot /tmp/test_calc.png"; sleep 2) | nc -q1 localhost 6416
```

- [ ] **Step 3: Run the existing stress test if available**

```bash
python3 test_recontrol_stress.py --no-launch --port 6416
```

- [ ] **Step 4: Verify debug commands**

```bash
(echo "break list"; sleep 2) | nc -q1 localhost 6416
(echo "watch status"; sleep 2) | nc -q1 localhost 6416
(echo "log list"; sleep 2) | nc -q1 localhost 6416
(echo "check list"; sleep 2) | nc -q1 localhost 6416
(echo "errorhandling list"; sleep 2) | nc -q1 localhost 6416
```

- [ ] **Step 5: Kill emulator**

```bash
pkill -f pose64 || true
```

---

### Task 9: Final cleanup and commit

**Files:**
- Modify: `src/core/ReControl.cpp` (remove any leftover dead code)

- [ ] **Step 1: Verify ReControl.cpp is now just infrastructure**

The file should contain only:
- Includes and globals (`gCPUWorker`, `gReControlServer`)
- `ReControlSession` class declaration (Q_OBJECT)
- `ReControlSession` constructor/destructor
- `Send`, `SendErr`, `QueueWork`, `QueueWorkResult`, `PauseProcessing`, `RunSubCommand`
- The dispatch table (`sCommandTable`)
- `DispatchCommand` (the table-driven loop)
- `OnReadyRead`, `OnDisconnected`, `OnSleepDone`, `ProcessBufferedCommands`
- `ReControlServer` class and implementation
- `ReControl_Startup`, `ReControl_Shutdown`
- `#include "ReControl.moc"`

Estimated size: ~350-450 lines (down from 3200).

- [ ] **Step 2: Remove any remaining dead code, unused includes**

- [ ] **Step 3: Build one final time**

```bash
cd build && cmake --build . -j$(nproc) 2>&1 | tail -5
```

- [ ] **Step 4: Final commit**

```bash
git add -A src/core/ReControl*.cpp src/core/ReControl.h
git commit -m "refactor: complete ReControl dispatch table + file split

ReControl.cpp split into 7 files with table-driven dispatch.
Command threading is now enforced by category, not ad-hoc per handler.
Fixes: launch main-thread deadlock, watch/spy/break status race
conditions, poke unusable during dialog.

See docs/superpowers/specs/2026-03-13-recontrol-refactor-design.md"
```
