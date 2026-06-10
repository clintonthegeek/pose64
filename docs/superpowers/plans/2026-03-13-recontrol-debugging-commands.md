# ReControl Debugging Commands Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose POSE64's buried debugging infrastructure (breakpoints, watchpoints, stack crawl, logging, gremlins, memory checks, error handling, profiling) as new ReControl TCP commands, making them accessible to AI developers via MCP.

**Architecture:** All new commands are added to `ReControlSession` in `src/core/ReControl.cpp`, following the existing pattern: declare `CmdFoo` method, implement it with argument parsing + `QueueWorkResult`/direct response, wire it into `DispatchCommand`. Commands that only read state (backtrace, break list, log list) work in `blocked_on_ui` by reading directly. Commands that modify CPU-visible state (break set, watch set) use `QueueWorkResult` with `EmSessionStopper`. Documentation updates go to `docs/recontrol-protocol.md`, `claude/skills/palm-dev/SKILL.md`, and `claude/agents/pose64-tester.md`.

**Tech Stack:** C++ (Qt6), Palm OS Emulator core APIs (DebugMgr, Logging, Hordes, MetaMemory, Profiling, EmPalmOS)

---

## File Structure

| File | Role |
|------|------|
| `src/core/ReControl.cpp` | All new command implementations (modify) |
| `docs/recontrol-protocol.md` | Protocol reference updates (modify) |
| `claude/skills/palm-dev/SKILL.md` | MCP skill documentation (modify) |
| `claude/agents/pose64-tester.md` | Tester agent documentation (modify) |
| `CMakeLists.txt` | Enable `HAS_PROFILING=1` (modify, Task 8 only) |

No new files are created. All commands are added to the existing `ReControlSession` class.

---

## Chunk 1: Stack Crawl and Breakpoints

### Task 1: `backtrace` command

**Files:**
- Modify: `src/core/ReControl.cpp` — add `#include "EmPalmOS.h"`, add `CmdBacktrace` declaration and implementation, wire into `DispatchCommand`

The `backtrace` (alias `bt`) command calls `EmPalmOS::GenerateStackCrawl()` and returns the frame list. Works in `blocked_on_ui` since CPU state is frozen.

**Protocol:**
```
backtrace
```
**Response (multi-line):**
```
OK backtrace
 #0 PC=00012340 A6=000FFFA0
 #1 PC=000118A2 A6=000FFFB8
 #2 PC=00010044 A6=000FFFD0
.
```

- [ ] **Step 1: Add include and method declaration**

In `ReControl.cpp`, add `#include "EmPalmOS.h"` near the other includes (after `#include "Patches/EmPatchState.h"`). Add `void CmdBacktrace (const QStringList& args);` to the `ReControlSession` class declaration (after `CmdDelete`).

- [ ] **Step 2: Implement CmdBacktrace**

Add before the `DispatchCommand` function:

```cpp
// ============================================================================
// CmdBacktrace — stack crawl
// ============================================================================

void ReControlSession::CmdBacktrace (const QStringList& args)
{
	if (!gSession) { SendErr ("transient", "no session"); return; }

	// Stack crawl reads memory — works in blocked_on_ui (frozen CPU)
	auto doBacktrace = []() -> std::string {
		CEnableFullAccess munge;
		EmStackFrameList frameList;
		EmPalmOS::GenerateStackCrawl (frameList);

		std::string result = "OK backtrace\n";
		for (size_t i = 0; i < frameList.size (); i++)
		{
			char buf[80];
			snprintf (buf, sizeof (buf), " #%zu PC=%08X A6=%08X\n",
				i,
				(unsigned) frameList[i].fAddressInFunction,
				(unsigned) frameList[i].fA6);
			result += buf;
		}
		result += ".\n";
		return result;
	};

	if (gSession->GetSessionState () == kBlockedOnUI)
	{
		Send (doBacktrace ());
		return;
	}

	QueueWorkResult ([doBacktrace]() -> std::string {
		if (!gSession) return "ERR transient: no session\n";
		EmSessionStopper stopper (gSession, kStopOnCycle);
		if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";
		return doBacktrace ();
	});
}
```

- [ ] **Step 3: Wire into DispatchCommand**

Add two entries in `DispatchCommand` (after the `"delete"` line):

```cpp
	else if (cmd == "backtrace")   CmdBacktrace (parts);
	else if (cmd == "bt")          CmdBacktrace (parts);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`
Expected: Successful build with no new errors.

- [ ] **Step 5: Manual test**

Run: `echo "backtrace" | nc -q1 localhost 6416`
Expected: Multi-line response with at least one `#0 PC=...` frame.

- [ ] **Step 6: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add backtrace/bt ReControl command for stack crawl"
```

---

### Task 2: `break` commands

**Files:**
- Modify: `src/core/ReControl.cpp` — add `#include "DebugMgr.h"`, add `CmdBreak` declaration and implementation

The `break` command manages instruction breakpoints (6 slots: indices 0-5, where 5 is the temp BP). Sub-commands: `list`, `set`, `clear`, `enable`, `disable`.

**Protocol:**
```
break list
break set <index> <addr> [condition]
break clear <index>
break enable <index>
break disable <index>
```

**Response examples:**
```
OK break list
 [0] enabled  addr=00012340 condition="d5.w == 0x1234"
 [1] disabled addr=00000000
 [2] disabled addr=00000000
 [3] disabled addr=00000000
 [4] disabled addr=00000000
 [5] disabled addr=00000000
.
```
```
OK
```

- [ ] **Step 1: Add include**

Add `#include "DebugMgr.h"` to the includes in `ReControl.cpp` (after `#include "EmPalmOS.h"` from Task 1).

- [ ] **Step 2: Add method declaration**

Add `void CmdBreak (const QStringList& args);` to the `ReControlSession` class declaration.

- [ ] **Step 3: Implement CmdBreak**

```cpp
// ============================================================================
// CmdBreak — manage instruction breakpoints
// ============================================================================

void ReControlSession::CmdBreak (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "break <list|set|clear|enable|disable> [args...]"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString sub = args[1].toLower ();

	if (sub == "list")
	{
		// List can read globals directly — no CPU interaction needed
		std::string result = "OK break list\n";
		for (int i = 0; i < dbgTotalBreakpoints; i++)
		{
			char buf[256];
			const char* condStr = "";
			if (gDebuggerGlobals.bpCondition[i] && gDebuggerGlobals.bpCondition[i]->source)
				condStr = gDebuggerGlobals.bpCondition[i]->source;

			snprintf (buf, sizeof (buf), " [%d] %s addr=%08X%s%s\n",
				i,
				gDebuggerGlobals.bp[i].enabled ? "enabled " : "disabled",
				(unsigned)(uintptr_t) gDebuggerGlobals.bp[i].addr,
				condStr[0] ? " condition=\"" : "",
				condStr[0] ? condStr : "");
			if (condStr[0])
			{
				std::string line (buf);
				// Remove trailing \n, append closing quote
				if (!line.empty () && line.back () == '\n') line.pop_back ();
				line += "\"\n";
				result += line;
			}
			else
			{
				result += buf;
			}
		}
		result += ".\n";
		Send (result);
		return;
	}

	if (sub == "set")
	{
		// break set <index> <addr> [condition...]
		if (args.size () < 4) { SendErr ("usage", "break set <index> <addr> [condition]"); return; }

		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
		{
			SendErr ("usage", "index must be 0-" + std::to_string (dbgTotalBreakpoints - 1));
			return;
		}

		std::string addrStr = args[3].toStdString ();
		emuptr addr;
		if (!ParseAddress (addrStr, addr))
		{
			SendErr ("usage", "invalid address '" + addrStr + "'");
			return;
		}

		// Optional condition string (remaining args joined)
		std::string condStr;
		for (int i = 4; i < args.size (); i++)
		{
			if (!condStr.empty ()) condStr += ' ';
			condStr += args[i].toStdString ();
		}

		BreakpointCondition* cond = nullptr;
		if (!condStr.empty ())
		{
			cond = Debug::NewBreakpointCondition (condStr.c_str ());
			if (!cond)
			{
				SendErr ("usage", "invalid condition '" + condStr + "'");
				return;
			}
		}

		QueueWorkResult ([index, addr, cond]() -> std::string {
			if (!gSession) { delete cond; return "ERR transient: no session\n"; }
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) { delete cond; return "ERR transient: could not stop session\n"; }

			Debug::SetBreakpoint (index, addr, cond);
			return "OK\n";
		});
		return;
	}

	if (sub == "clear")
	{
		if (args.size () < 3) { SendErr ("usage", "break clear <index>"); return; }
		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
		{
			SendErr ("usage", "index must be 0-" + std::to_string (dbgTotalBreakpoints - 1));
			return;
		}

		QueueWorkResult ([index]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			Debug::ClearBreakpoint (index);  // also deletes condition internally
			return "OK\n";
		});
		return;
	}

	if (sub == "enable" || sub == "disable")
	{
		if (args.size () < 3) { SendErr ("usage", "break " + sub.toStdString () + " <index>"); return; }
		int index = args[2].toInt ();
		if (index < 0 || index >= dbgTotalBreakpoints)
		{
			SendErr ("usage", "index must be 0-" + std::to_string (dbgTotalBreakpoints - 1));
			return;
		}

		bool enable = (sub == "enable");
		QueueWorkResult ([index, enable]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			gDebuggerGlobals.bp[index].enabled = enable;
			return "OK\n";
		});
		return;
	}

	SendErr ("usage", "break <list|set|clear|enable|disable> [args...]");
}
```

- [ ] **Step 4: Wire into DispatchCommand**

Add after the `"bt"` entry:

```cpp
	else if (cmd == "break")       CmdBreak (parts);
```

- [ ] **Step 5: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`
Expected: Successful build.

- [ ] **Step 6: Manual test**

```bash
echo "break list" | nc -q1 localhost 6416
echo "break set 0 0x12340" | nc -q1 localhost 6416
echo "break list" | nc -q1 localhost 6416
echo "break clear 0" | nc -q1 localhost 6416
```
Expected: List shows 6 slots, set shows OK, list shows enabled, clear shows OK.

- [ ] **Step 7: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add break set/clear/list/enable/disable ReControl commands"
```

---

## Chunk 2: Watch, Spy, and Logging

### Task 3: `watch` and `spy` commands

**Files:**
- Modify: `src/core/ReControl.cpp` — add `CmdWatch` and `CmdSpy` methods

**Protocol:**
```
watch set <addr> <nbytes>
watch clear
watch status
spy set <addr>
spy clear
spy status
```

- [ ] **Step 1: Add method declarations**

Add to `ReControlSession` class:
```cpp
	void CmdWatch (const QStringList& args);
	void CmdSpy (const QStringList& args);
```

- [ ] **Step 2: Implement CmdWatch**

```cpp
// ============================================================================
// CmdWatch — data watchpoints
// ============================================================================

void ReControlSession::CmdWatch (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "watch <set|clear|status>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		char buf[128];
		if (gDebuggerGlobals.watchEnabled)
			snprintf (buf, sizeof (buf), "OK watch enabled addr=%08X nbytes=%u\n",
				(unsigned) gDebuggerGlobals.watchAddr,
				(unsigned) gDebuggerGlobals.watchBytes);
		else
			snprintf (buf, sizeof (buf), "OK watch disabled\n");
		Send (buf);
		return;
	}

	if (sub == "clear")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			gDebuggerGlobals.watchEnabled = false;
			gDebuggerGlobals.watchAddr = 0;
			gDebuggerGlobals.watchBytes = 0;
			return "OK\n";
		});
		return;
	}

	if (sub == "set")
	{
		if (args.size () < 4) { SendErr ("usage", "watch set <addr> <nbytes>"); return; }

		std::string addrStr = args[2].toStdString ();
		int nbytes = args[3].toInt ();
		if (nbytes < 1 || nbytes > 65536) { SendErr ("usage", "nbytes must be 1-65536"); return; }

		QueueWorkResult ([addrStr, nbytes]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			emuptr addr;
			if (!ParseAddress (addrStr, addr))
				return "ERR usage: invalid address '" + addrStr + "'\n";

			gDebuggerGlobals.watchEnabled = true;
			gDebuggerGlobals.watchAddr = addr;
			gDebuggerGlobals.watchBytes = nbytes;
			return "OK\n";
		});
		return;
	}

	SendErr ("usage", "watch <set|clear|status>");
}
```

- [ ] **Step 3: Implement CmdSpy**

```cpp
// ============================================================================
// CmdSpy — step spy (single-address value-change monitor)
// ============================================================================

void ReControlSession::CmdSpy (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "spy <set|clear|status>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		char buf[128];
		if (gDebuggerGlobals.stepSpy)
			snprintf (buf, sizeof (buf), "OK spy enabled addr=%08X value=%08X\n",
				(unsigned) gDebuggerGlobals.ssAddr,
				(unsigned) gDebuggerGlobals.ssValue);
		else
			snprintf (buf, sizeof (buf), "OK spy disabled\n");
		Send (buf);
		return;
	}

	if (sub == "clear")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			gDebuggerGlobals.stepSpy = false;
			gDebuggerGlobals.ssAddr = 0;
			gDebuggerGlobals.ssValue = 0;
			return "OK\n";
		});
		return;
	}

	if (sub == "set")
	{
		if (args.size () < 3) { SendErr ("usage", "spy set <addr>"); return; }
		std::string addrStr = args[2].toStdString ();

		QueueWorkResult ([addrStr]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnCycle);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			emuptr addr;
			if (!ParseAddress (addrStr, addr))
				return "ERR usage: invalid address '" + addrStr + "'\n";

			CEnableFullAccess munge;
			gDebuggerGlobals.stepSpy = true;
			gDebuggerGlobals.ssAddr = addr;
			gDebuggerGlobals.ssValue = EmMemGet32 (addr);
			return "OK\n";
		});
		return;
	}

	SendErr ("usage", "spy <set|clear|status>");
}
```

- [ ] **Step 4: Wire into DispatchCommand**

Add after the `"break"` entry:
```cpp
	else if (cmd == "watch")       CmdWatch (parts);
	else if (cmd == "spy")         CmdSpy (parts);
```

- [ ] **Step 5: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`

- [ ] **Step 6: Manual test**

```bash
echo "watch status" | nc -q1 localhost 6416
echo "spy status" | nc -q1 localhost 6416
```
Expected: Both report `disabled`.

- [ ] **Step 7: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add watch/spy ReControl commands for data watchpoints"
```

---

### Task 4: `log` commands

**Files:**
- Modify: `src/core/ReControl.cpp` — add `#include "Logging.h"` (already included transitively but be explicit), add `CmdLog` method

The logging system uses the `Preference<uint8>` template. Values: 0=off, 1=gremlin-only, 2=always. Category names map to `kPrefKeyLog*` preference keys.

**Protocol:**
```
log list
log set <category> <0|1|2>
log dump [path]
log clear
```

**Response (list):**
```
OK log list
 ErrorMessages=2
 WarningMessages=2
 Gremlins=0
 CPUOpcodes=0
 ...
.
```

- [ ] **Step 1: Add method declaration**

Add `void CmdLog (const QStringList& args);` to the class.

- [ ] **Step 2: Implement CmdLog**

```cpp
// ============================================================================
// CmdLog — logging category control
// ============================================================================

void ReControlSession::CmdLog (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "log <list|set|dump|clear>"); return; }

	QString sub = args[1].toLower ();

	// Table of category names and their preference keys
	static const struct {
		const char* name;
		PrefKeyType key;
	} kLogCategories[] = {
		{ "ErrorMessages",     kPrefKeyLogErrorMessages     },
		{ "WarningMessages",   kPrefKeyLogWarningMessages   },
		{ "Gremlins",          kPrefKeyLogGremlins           },
		{ "CPUOpcodes",        kPrefKeyLogCPUOpcodes         },
		{ "EnqueuedEvents",    kPrefKeyLogEnqueuedEvents     },
		{ "DequeuedEvents",    kPrefKeyLogDequeuedEvents     },
		{ "SystemCalls",       kPrefKeyLogSystemCalls        },
		{ "ApplicationCalls",  kPrefKeyLogApplicationCalls   },
		{ "Serial",            kPrefKeyLogSerial             },
		{ "SerialData",        kPrefKeyLogSerialData         },
		{ "NetLib",            kPrefKeyLogNetLib             },
		{ "NetLibData",        kPrefKeyLogNetLibData         },
		{ "ExgMgr",            kPrefKeyLogExgMgr             },
		{ "ExgMgrData",        kPrefKeyLogExgMgrData         },
		{ "HLDebugger",        kPrefKeyLogHLDebugger         },
		{ "HLDebuggerData",    kPrefKeyLogHLDebuggerData     },
		{ "LLDebugger",        kPrefKeyLogLLDebugger         },
		{ "LLDebuggerData",    kPrefKeyLogLLDebuggerData     },
		{ "RPC",               kPrefKeyLogRPC                },
		{ "RPCData",           kPrefKeyLogRPCData            },
	};
	const int kNumCategories = sizeof (kLogCategories) / sizeof (kLogCategories[0]);

	if (sub == "list")
	{
		std::string result = "OK log list\n";
		for (int i = 0; i < kNumCategories; i++)
		{
			Preference<uint8> pref (kLogCategories[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%d\n", kLogCategories[i].name, (int) *pref);
			result += buf;
		}
		result += ".\n";
		Send (result);
		return;
	}

	if (sub == "set")
	{
		if (args.size () < 4) { SendErr ("usage", "log set <category> <0|1|2>"); return; }

		std::string catName = args[2].toStdString ();
		int value = args[3].toInt ();
		if (value < 0 || value > 2) { SendErr ("usage", "value must be 0, 1, or 2"); return; }

		// Find category (case-insensitive)
		bool found = false;
		for (int i = 0; i < kNumCategories; i++)
		{
			if (strcasecmp (catName.c_str (), kLogCategories[i].name) == 0)
			{
				Preference<uint8> pref (kLogCategories[i].key);
				pref = (uint8) value;
				found = true;
				break;
			}
		}

		if (!found)
		{
			SendErr ("usage", "unknown category '" + catName + "'");
			return;
		}

		Send ("OK\n");
		return;
	}

	if (sub == "dump")
	{
		LogDump ();
		Send ("OK\n");
		return;
	}

	if (sub == "clear")
	{
		LogClear ();
		Send ("OK\n");
		return;
	}

	SendErr ("usage", "log <list|set|dump|clear>");
}
```

- [ ] **Step 3: Wire into DispatchCommand**

```cpp
	else if (cmd == "log")         CmdLog (parts);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`

- [ ] **Step 5: Manual test**

```bash
echo "log list" | nc -q1 localhost 6416
echo "log set SystemCalls 2" | nc -q1 localhost 6416
echo "log list" | nc -q1 localhost 6416
```
Expected: SystemCalls changes from 0 to 2 in the list.

- [ ] **Step 6: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add log list/set/dump/clear ReControl commands"
```

---

## Chunk 3: Gremlins, Memory Checks, and Error Handling

### Task 5: `gremlin` commands

**Files:**
- Modify: `src/core/ReControl.cpp` — add `#include "Hordes.h"`, `#include "CGremlins.h"`, add `CmdGremlin` method

Gremlins use `Hordes::NewGremlin()` (single gremlin), `Hordes::Suspend/Step/Resume/Stop()`, and `Hordes::Status()`. The `NewGremlin` call requires `EmSessionStopper (kStopOnSysCall)` as shown in `EmDocument::DoGremlinNew`.

**Protocol:**
```
gremlin new <seed> <events>
gremlin status
gremlin suspend
gremlin step
gremlin resume
gremlin stop
```

**Response (status):**
```
OK gremlin running number=42 step=1234 until=10000
```
or
```
OK gremlin off
```

- [ ] **Step 1: Add includes and method declaration**

Add `#include "Hordes.h"` and `#include "CGremlins.h"` to the includes (if not already present transitively). Add `void CmdGremlin (const QStringList& args);` to the class.

- [ ] **Step 2: Implement CmdGremlin**

```cpp
// ============================================================================
// CmdGremlin — automated stress testing
// ============================================================================

void ReControlSession::CmdGremlin (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "gremlin <new|status|suspend|step|resume|stop>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString sub = args[1].toLower ();

	if (sub == "status")
	{
		if (!Hordes::IsOn ())
		{
			Send ("OK gremlin off\n");
			return;
		}

		unsigned short number;
		UInt32 step, until;
		Hordes::Status (&number, &step, &until);

		char buf[128];
		snprintf (buf, sizeof (buf), "OK gremlin running number=%d step=%u until=%u\n",
			(int) number, (unsigned) step, (unsigned) until);
		Send (buf);
		return;
	}

	if (sub == "new")
	{
		if (args.size () < 4) { SendErr ("usage", "gremlin new <seed> <events>"); return; }
		if (Hordes::IsOn ()) { SendErr ("transient", "gremlin already running"); return; }

		int seed = args[2].toInt ();
		int events = args[3].toInt ();
		if (events < 1) { SendErr ("usage", "events must be > 0"); return; }

		QueueWorkResult ([seed, events]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopOnSysCall);
			if (!stopper.Stopped ()) return "ERR transient: could not stop session\n";

			GremlinInfo info;
			info.fNumber = seed;
			info.fSteps = events;
			info.fFinal = events;
			info.fSaveFrequency = 10000;
			info.fAppList = gGremlinAppList;  // use current app list

			Hordes::NewGremlin (info);

			char buf[80];
			snprintf (buf, sizeof (buf), "OK gremlin started seed=%d events=%d\n", seed, events);
			return std::string (buf);
		});
		return;
	}

	if (sub == "suspend")
	{
		if (!Hordes::IsOn ()) { SendErr ("transient", "no gremlin running"); return; }
		if (!Hordes::CanSuspend ()) { SendErr ("transient", "cannot suspend now"); return; }
		QueueWork ([](){ Hordes::Suspend (); });
		return;
	}

	if (sub == "step")
	{
		if (!Hordes::IsOn ()) { SendErr ("transient", "no gremlin running"); return; }
		if (!Hordes::CanStep ()) { SendErr ("transient", "cannot step now"); return; }
		QueueWork ([](){ Hordes::Step (); });
		return;
	}

	if (sub == "resume")
	{
		if (!Hordes::IsOn ()) { SendErr ("transient", "no gremlin running"); return; }
		if (!Hordes::CanResume ()) { SendErr ("transient", "cannot resume now"); return; }
		QueueWork ([](){ Hordes::Resume (); });
		return;
	}

	if (sub == "stop")
	{
		if (!Hordes::IsOn ()) { SendErr ("transient", "no gremlin running"); return; }
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			Hordes::Stop ();
			return "OK\n";
		});
		return;
	}

	SendErr ("usage", "gremlin <new|status|suspend|step|resume|stop>");
}
```

- [ ] **Step 3: Wire into DispatchCommand**

```cpp
	else if (cmd == "gremlin")     CmdGremlin (parts);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`

- [ ] **Step 5: Manual test**

```bash
echo "gremlin status" | nc -q1 localhost 6416
```
Expected: `OK gremlin off`

- [ ] **Step 6: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add gremlin new/status/suspend/step/resume/stop ReControl commands"
```

---

### Task 6: `check` commands (MetaMemory report flags)

**Files:**
- Modify: `src/core/ReControl.cpp` — add `CmdCheck` method

The 18 report flags are boolean preferences accessed via `Preference<bool>`. The `FOR_EACH_REPORT_PREF` macro in `Logging.h` lists all names.

**Protocol:**
```
check list
check set <flag> <on|off>
check set-all <on|off>
```

- [ ] **Step 1: Add method declaration**

Add `void CmdCheck (const QStringList& args);` to the class.

- [ ] **Step 2: Implement CmdCheck**

```cpp
// ============================================================================
// CmdCheck — MetaMemory report flags
// ============================================================================

void ReControlSession::CmdCheck (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "check <list|set|set-all>"); return; }

	static const struct {
		const char* name;
		PrefKeyType key;
	} kReportFlags[] = {
		{ "FreeChunkAccess",         kPrefKeyReportFreeChunkAccess         },
		{ "HardwareRegisterAccess",  kPrefKeyReportHardwareRegisterAccess  },
		{ "LowMemoryAccess",         kPrefKeyReportLowMemoryAccess         },
		{ "LowStackAccess",          kPrefKeyReportLowStackAccess          },
		{ "MemMgrDataAccess",        kPrefKeyReportMemMgrDataAccess        },
		{ "MemMgrLeaks",             kPrefKeyReportMemMgrLeaks             },
		{ "MemMgrSemaphore",         kPrefKeyReportMemMgrSemaphore         },
		{ "OffscreenObject",         kPrefKeyReportOffscreenObject         },
		{ "OverlayErrors",           kPrefKeyReportOverlayErrors           },
		{ "ProscribedFunction",      kPrefKeyReportProscribedFunction      },
		{ "ROMAccess",               kPrefKeyReportROMAccess               },
		{ "ScreenAccess",            kPrefKeyReportScreenAccess            },
		{ "SizelessObject",          kPrefKeyReportSizelessObject          },
		{ "StackAlmostOverflow",     kPrefKeyReportStackAlmostOverflow     },
		{ "StrictIntlChecks",        kPrefKeyReportStrictIntlChecks        },
		{ "SystemGlobalAccess",      kPrefKeyReportSystemGlobalAccess      },
		{ "UIMgrDataAccess",         kPrefKeyReportUIMgrDataAccess         },
		{ "UnlockedChunkAccess",     kPrefKeyReportUnlockedChunkAccess     },
	};
	const int kNumFlags = sizeof (kReportFlags) / sizeof (kReportFlags[0]);

	QString sub = args[1].toLower ();

	if (sub == "list")
	{
		std::string result = "OK check list\n";
		for (int i = 0; i < kNumFlags; i++)
		{
			Preference<bool> pref (kReportFlags[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%s\n",
				kReportFlags[i].name, *pref ? "on" : "off");
			result += buf;
		}
		result += ".\n";
		Send (result);
		return;
	}

	if (sub == "set-all")
	{
		if (args.size () < 3) { SendErr ("usage", "check set-all <on|off>"); return; }
		QString val = args[2].toLower ();
		if (val != "on" && val != "off") { SendErr ("usage", "check set-all <on|off>"); return; }

		bool enable = (val == "on");
		for (int i = 0; i < kNumFlags; i++)
		{
			Preference<bool> pref (kReportFlags[i].key);
			pref = enable;
		}
		Send ("OK\n");
		return;
	}

	if (sub == "set")
	{
		if (args.size () < 4) { SendErr ("usage", "check set <flag> <on|off>"); return; }

		std::string flagName = args[2].toStdString ();
		QString val = args[3].toLower ();
		if (val != "on" && val != "off") { SendErr ("usage", "check set <flag> <on|off>"); return; }

		bool enable = (val == "on");
		bool found = false;
		for (int i = 0; i < kNumFlags; i++)
		{
			if (strcasecmp (flagName.c_str (), kReportFlags[i].name) == 0)
			{
				Preference<bool> pref (kReportFlags[i].key);
				pref = enable;
				found = true;
				break;
			}
		}

		if (!found) { SendErr ("usage", "unknown flag '" + flagName + "'"); return; }
		Send ("OK\n");
		return;
	}

	SendErr ("usage", "check <list|set|set-all>");
}
```

- [ ] **Step 3: Wire into DispatchCommand**

```cpp
	else if (cmd == "check")       CmdCheck (parts);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`

- [ ] **Step 5: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add check list/set/set-all ReControl commands for MetaMemory flags"
```

---

### Task 7: `errorhandling` commands

**Files:**
- Modify: `src/core/ReControl.cpp` — add `CmdErrorHandling` method

Four settings: `WarningOff`, `ErrorOff`, `WarningOn`, `ErrorOn`. Each is `Preference<EmErrorHandlingOption>` with values `kShow`, `kContinue`, `kQuit`, `kSwitch`.

**Protocol:**
```
errorhandling get
errorhandling set <setting> <show|continue|quit|switch>
```

- [ ] **Step 1: Add method declaration**

Add `void CmdErrorHandling (const QStringList& args);` to the class.

- [ ] **Step 2: Implement CmdErrorHandling**

```cpp
// ============================================================================
// CmdErrorHandling — error/warning behavior configuration
// ============================================================================

void ReControlSession::CmdErrorHandling (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "errorhandling <get|set>"); return; }

	static const struct {
		const char* name;
		PrefKeyType key;
	} kSettings[] = {
		{ "WarningOff", kPrefKeyWarningOff },
		{ "ErrorOff",   kPrefKeyErrorOff   },
		{ "WarningOn",  kPrefKeyWarningOn  },
		{ "ErrorOn",    kPrefKeyErrorOn    },
	};
	const int kNumSettings = sizeof (kSettings) / sizeof (kSettings[0]);

	static const struct {
		const char* name;
		EmErrorHandlingOption value;
	} kOptions[] = {
		{ "show",     kShow     },
		{ "continue", kContinue },
		{ "quit",     kQuit     },
		{ "switch",   kSwitch   },
	};
	const int kNumOptions = sizeof (kOptions) / sizeof (kOptions[0]);

	auto optionName = [&](EmErrorHandlingOption opt) -> const char* {
		for (int i = 0; i < kNumOptions; i++)
			if (kOptions[i].value == opt) return kOptions[i].name;
		return "unknown";
	};

	QString sub = args[1].toLower ();

	if (sub == "get")
	{
		std::string result = "OK errorhandling\n";
		for (int i = 0; i < kNumSettings; i++)
		{
			Preference<EmErrorHandlingOption> pref (kSettings[i].key);
			char buf[64];
			snprintf (buf, sizeof (buf), " %s=%s\n",
				kSettings[i].name, optionName (*pref));
			result += buf;
		}
		result += ".\n";
		Send (result);
		return;
	}

	if (sub == "set")
	{
		if (args.size () < 4) { SendErr ("usage", "errorhandling set <setting> <show|continue|quit|switch>"); return; }

		std::string settingName = args[2].toStdString ();
		std::string optName = args[3].toLower ().toStdString ();

		// Find setting
		bool foundSetting = false;
		PrefKeyType key = kPrefKeyWarningOff;  // initialized to valid default
		for (int i = 0; i < kNumSettings; i++)
		{
			if (strcasecmp (settingName.c_str (), kSettings[i].name) == 0)
			{
				key = kSettings[i].key;
				foundSetting = true;
				break;
			}
		}
		if (!foundSetting) { SendErr ("usage", "unknown setting '" + settingName + "'"); return; }

		// Find option
		bool foundOption = false;
		EmErrorHandlingOption opt = kShow;  // initialized to valid default
		for (int i = 0; i < kNumOptions; i++)
		{
			if (optName == kOptions[i].name)
			{
				opt = kOptions[i].value;
				foundOption = true;
				break;
			}
		}
		if (!foundOption)
		{
			SendErr ("usage", "unknown option '" + optName + "' (use show/continue/quit/switch)");
			return;
		}

		Preference<EmErrorHandlingOption> pref (key);
		pref = opt;
		Send ("OK\n");
		return;
	}

	SendErr ("usage", "errorhandling <get|set>");
}
```

- [ ] **Step 3: Wire into DispatchCommand**

```cpp
	else if (cmd == "errorhandling") CmdErrorHandling (parts);
```

- [ ] **Step 4: Build and verify**

Run: `cd build && cmake --build . -j$(nproc) 2>&1 | tail -5`

- [ ] **Step 5: Commit**

```bash
git add src/core/ReControl.cpp
git commit -m "feat: add errorhandling get/set ReControl commands"
```

---

## Chunk 4: Profiling and Documentation

### Task 8: `profile` commands (requires build change)

**Files:**
- Modify: `CMakeLists.txt:46` — change `HAS_PROFILING=0` to `HAS_PROFILING=1`
- Modify: `src/core/ReControl.cpp` — add `#include "Profiling.h"`, add `CmdProfile` method inside `#if HAS_PROFILING`

The profiling API: `ProfileInit(maxCalls, maxDepth)`, `ProfileStart()`, `ProfileStop()`, `ProfileDump(filename)`, `ProfilePrint(filename)`, `ProfileCleanup()`. Global counters: `gClockCycles`, `gReadCycles`, `gWriteCycles`.

**Protocol:**
```
profile init [maxcalls] [maxdepth]
profile start
profile stop
profile dump <path>
profile print <path>
profile cleanup
profile cycles
```

- [ ] **Step 1: Enable HAS_PROFILING in CMakeLists.txt**

Change line 46 from `HAS_PROFILING=0` to `HAS_PROFILING=1`.

- [ ] **Step 2: Add include and method declaration**

Add `#include "Profiling.h"` to the includes. Add `void CmdProfile (const QStringList& args);` to the class.

- [ ] **Step 3: Implement CmdProfile**

```cpp
// ============================================================================
// CmdProfile — CPU profiling
// ============================================================================

#if HAS_PROFILING
void ReControlSession::CmdProfile (const QStringList& args)
{
	if (args.size () < 2) { SendErr ("usage", "profile <init|start|stop|dump|print|cleanup|cycles>"); return; }
	if (!gSession) { SendErr ("transient", "no session"); return; }

	QString sub = args[1].toLower ();

	if (sub == "init")
	{
		int maxCalls = MAXFNCALLS;
		int maxDepth = 200;
		if (args.size () >= 3) maxCalls = args[2].toInt ();
		if (args.size () >= 4) maxDepth = args[3].toInt ();
		if (maxCalls < 1) maxCalls = MAXFNCALLS;
		if (maxDepth < 1) maxDepth = 200;

		QueueWorkResult ([maxCalls, maxDepth]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfileInit (maxCalls, maxDepth);
			return "OK\n";
		});
		return;
	}

	if (sub == "start")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfileStart ();
			return "OK\n";
		});
		return;
	}

	if (sub == "stop")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfileStop ();
			return "OK\n";
		});
		return;
	}

	if (sub == "dump")
	{
		if (args.size () < 3) { SendErr ("usage", "profile dump <path>"); return; }
		std::string path = args[2].toStdString ();

		QueueWorkResult ([path]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfileDump (path.c_str ());
			return "OK\n";
		});
		return;
	}

	if (sub == "print")
	{
		if (args.size () < 3) { SendErr ("usage", "profile print <path>"); return; }
		std::string path = args[2].toStdString ();

		QueueWorkResult ([path]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfilePrint (path.c_str ());
			return "OK\n";
		});
		return;
	}

	if (sub == "cleanup")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);
			ProfileCleanup ();
			return "OK\n";
		});
		return;
	}

	if (sub == "cycles")
	{
		QueueWorkResult ([]() -> std::string {
			if (!gSession) return "ERR transient: no session\n";
			EmSessionStopper stopper (gSession, kStopNow);

			char buf[128];
			snprintf (buf, sizeof (buf), "OK clock=%lld read=%lld write=%lld\n",
				(long long) gClockCycles, (long long) gReadCycles, (long long) gWriteCycles);
			return std::string (buf);
		});
		return;
	}

	SendErr ("usage", "profile <init|start|stop|dump|print|cleanup|cycles>");
}
#endif // HAS_PROFILING
```

- [ ] **Step 4: Wire into DispatchCommand**

```cpp
#if HAS_PROFILING
	else if (cmd == "profile")     CmdProfile (parts);
#endif
```

- [ ] **Step 5: Full rebuild (HAS_PROFILING changed)**

Run: `cd build && cmake .. && cmake --build . -j$(nproc) 2>&1 | tail -10`
Expected: Full rebuild succeeds (profiling code now compiled).

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt src/core/ReControl.cpp
git commit -m "feat: enable profiling (HAS_PROFILING=1) and add profile ReControl commands"
```

---

### Task 9: Documentation updates

**Files:**
- Modify: `docs/recontrol-protocol.md`
- Modify: `claude/skills/palm-dev/SKILL.md`
- Modify: `claude/agents/pose64-tester.md`

- [ ] **Step 1: Update recontrol-protocol.md**

Add new section before the "Coordinate Systems" section. Add entries to the Commands section tables.

Add to the **Commands** section, new table **Debugging**:

```markdown
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
| `log list` | Multi-line | List 20 logging categories with current values |
| `log set <cat> <0\|1\|2>` | `OK\n` | Set logging level (0=off, 1=gremlin, 2=always) |
| `log dump` | `OK\n` | Flush log buffer to file |
| `log clear` | `OK\n` | Clear log buffer |
| `gremlin new <seed> <events>` | `OK ...\n` | Start automated stress test |
| `gremlin status` | `OK ...\n` | Query gremlin progress |
| `gremlin suspend` | `OK\n` | Pause gremlin |
| `gremlin step` | `OK\n` | Single-step gremlin |
| `gremlin resume` | `OK\n` | Resume gremlin |
| `gremlin stop` | `OK\n` | Stop gremlin |
| `check list` | Multi-line | List 18 memory-check flags with on/off status |
| `check set <flag> <on\|off>` | `OK\n` | Toggle individual memory check |
| `check set-all <on\|off>` | `OK\n` | Toggle all memory checks |
| `errorhandling get` | Multi-line | Query error/warning behavior settings |
| `errorhandling set <s> <opt>` | `OK\n` | Set behavior (show/continue/quit/switch) |
| `profile init [max] [depth]` | `OK\n` | Initialize profiler |
| `profile start` | `OK\n` | Begin profiling |
| `profile stop` | `OK\n` | Pause profiling |
| `profile dump <path>` | `OK\n` | Write Metrowerks .mwp profile |
| `profile print <path>` | `OK\n` | Write text profile report |
| `profile cleanup` | `OK\n` | Free profiler memory |
| `profile cycles` | `OK ...\n` | Query cycle counters |
```

Also update the `blocked_on_ui` section to mention `backtrace` works there.

- [ ] **Step 2: Update SKILL.md**

Add a new section **Debugging Commands** to `claude/skills/palm-dev/SKILL.md` after the "Memory Inspection" section:

```markdown
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
```

- [ ] **Step 3: Update pose64-tester.md**

Add to the **Tools** section list:
```
**Debugging:** `palm_backtrace` (or `palm_bt`), `palm_break`, `palm_watch`, `palm_spy`, `palm_log`, `palm_gremlin`, `palm_check`, `palm_errorhandling`, `palm_profile`
```

Add to the **Quick Reference** section:
```
**Stack trace:** `palm_backtrace` (also `palm_bt`) — works in `blocked_on_ui`
**Set breakpoint:** `palm_break set 0 0x12340`
**Enable logging:** `palm_log set SystemCalls 2`
**Run gremlin:** `palm_gremlin new 42 10000`
**Enable checks:** `palm_check set-all on`
```

Update step 6 of the **Workflow** to mention:
```
   - Use `palm_backtrace` for stack traces when the app crashes
   - Use `palm_break` to set breakpoints for targeted debugging
```

- [ ] **Step 4: Commit documentation**

```bash
git add docs/recontrol-protocol.md claude/skills/palm-dev/SKILL.md claude/agents/pose64-tester.md
git commit -m "docs: document new debugging ReControl commands in protocol, skill, and agent"
```

---

## Dependency Graph

```
Task 1 (backtrace) ─┐
Task 2 (break)     ─┤─→ Task 9 (documentation)
Task 3 (watch/spy) ─┤
Task 4 (log)       ─┤
Task 5 (gremlin)   ─┤
Task 6 (check)     ─┤
Task 7 (errorhdl)  ─┤
Task 8 (profile)   ─┘
```

Tasks 1-8 are independent of each other (all modify different sections of ReControl.cpp) but must all complete before Task 9 (documentation). Tasks 1-8 can be parallelized across subagents if available, with a single final documentation pass.

## Threading Considerations

- **Read-only commands** (`backtrace`, `break list`, `watch status`, `spy status`, `log list`, `gremlin status`, `check list`, `errorhandling get`): Can execute directly on UI thread. For `backtrace`, detect `blocked_on_ui` and read directly.
- **State-modifying commands** (`break set/clear/enable/disable`, `watch set/clear`, `spy set/clear`, `gremlin new/stop`, `profile *`): Use `QueueWorkResult` with `EmSessionStopper` since they modify CPU-visible state or read counters that the CPU thread writes.
- **Preference-only commands** (`log set`, `check set`, `errorhandling set`): Preferences are thread-safe (have internal mutex), can respond directly.
