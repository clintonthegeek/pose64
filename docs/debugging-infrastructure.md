# POSE64 Debugging Infrastructure — Gap Analysis (2026-03-13, morning)

> **STATUS BANNER (added 2026-06-09).** This is the gap analysis written the
> MORNING of 2026-03-13 (03:35). Its §9 recommendations were implemented the
> same day (commits eaf9e1f, 78a8f1e), which inverts much of what it says:
>
> - ReControl now has **36 commands**, including backtrace/break/watch/spy/
>   log/gremlin/check/errorhandling/profile — §2's command count and §8's
>   "ReControl: NONE" rows are obsolete. Live list: `docs/recontrol-protocol.md`.
> - Profiling is compiled **IN** (`HAS_PROFILING=1`, CMakeLists.txt:47) with a
>   `profile` command — §5/§7.4/§8 say the opposite.
> - There is no `dbs` ReControl command (§2 lists one); the MCP `palm_dbs`
>   tool maps to `apps all` inside the proxy.
> - Logging has 20 categories (not 25).
>
> Still TRUE and important: the **MCP column** — none of the debugging
> commands are exposed as MCP tools; they are TCP-only. Also true (verified
> 2026-06-09): a `break` hit only stops execution if an external SLP debugger
> is attached; otherwise it silently continues (DebugMgr.cpp,
> `ConditionalBreak`/`EnterDebugger`). Section 7's internals documentation
> remains the best reference of its kind; read everything else as history.

This document catalogs every debugging, profiling, and diagnostic feature in
POSE64, including features inherited from the original Palm OS Emulator (POSE)
that may not be accessible through the current UI or remote control interface.

The purpose of this document is self-awareness: knowing what we have, what
works, what's broken, and what's invisible to remote AI developers using the
MCP/ReControl interface.

---

## Table of Contents

1. [Architecture Overview](#1-architecture-overview)
2. [What's Exposed via ReControl/MCP](#2-whats-exposed-via-recontrolmcp)
3. [What's Exposed via Qt GUI Only](#3-whats-exposed-via-qt-gui-only)
4. [What Exists in Code but Is Inaccessible](#4-what-exists-in-code-but-is-inaccessible)
5. [What's Compiled Out](#5-whats-compiled-out)
6. [What Was Never Ported from FLTK](#6-what-was-never-ported-from-fltk)
7. [Feature Details](#7-feature-details)
   - [7.1 Breakpoint System](#71-breakpoint-system)
   - [7.2 Watchpoints and Step Spy](#72-watchpoints-and-step-spy)
   - [7.3 Stack Crawl / Backtrace](#73-stack-crawl--backtrace)
   - [7.4 Profiling](#74-profiling)
   - [7.5 Logging](#75-logging)
   - [7.6 Memory Diagnostics (MetaMemory)](#76-memory-diagnostics-metamemory)
   - [7.7 Error Handling Configuration](#77-error-handling-configuration)
   - [7.8 Gremlins / Hordes](#78-gremlins--hordes)
   - [7.9 Tracer](#79-tracer)
   - [7.10 CPU State and History](#710-cpu-state-and-history)
   - [7.11 ROM Function Calling](#711-rom-function-calling)
   - [7.12 Session State Debugging](#712-session-state-debugging)
8. [Status Matrix](#8-status-matrix)
9. [Priority Recommendations](#9-priority-recommendations)

---

## 1. Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                    AI Developer (Claude)                  │
│                          │                               │
│                   MCP JSON-RPC (stdio)                   │
│                          │                               │
│               pose64-mcp-proxy (C++)                     │
│                          │                               │
│                    TCP localhost:6416                     │
│                          │                               │
│  ┌───────────────────────▼────────────────────────────┐  │
│  │              ReControl TCP Server                  │  │
│  │           (28 commands implemented)                 │  │
│  │                                                    │  │
│  │  ┌──────────────────────────────────────────────┐  │  │
│  │  │          POSE64 Emulator Core                │  │  │
│  │  │                                              │  │  │
│  │  │  ┌─────────┐  ┌──────────┐  ┌────────────┐  │  │  │
│  │  │  │DebugMgr │  │Profiling │  │  Logging   │  │  │  │
│  │  │  │(6 BPs)  │  │(cycles)  │  │(25 cats)   │  │  │  │
│  │  │  └─────────┘  └──────────┘  └────────────┘  │  │  │
│  │  │  ┌─────────┐  ┌──────────┐  ┌────────────┐  │  │  │
│  │  │  │MetaMem  │  │ Hordes/  │  │  Tracer    │  │  │  │
│  │  │  │(access) │  │ Gremlins │  │(instrs)    │  │  │  │
│  │  │  └─────────┘  └──────────┘  └────────────┘  │  │  │
│  │  │  ┌─────────┐  ┌──────────┐  ┌────────────┐  │  │  │
│  │  │  │ErrorHdl │  │StackCrawl│  │ ROMStubs   │  │  │  │
│  │  │  │(50+ err)│  │(backtrce)│  │(100+ fns)  │  │  │  │
│  │  │  └─────────┘  └──────────┘  └────────────┘  │  │  │
│  │  └──────────────────────────────────────────────┘  │  │
│  └────────────────────────────────────────────────────┘  │
│                          │                               │
│                   Qt GUI (local only)                    │
│           Debugging | Logging | Error Handling           │
│           (NO: Breakpoints | Profiling | Tracer)         │
└──────────────────────────────────────────────────────────┘
```

The AI developer can only reach the ReControl layer.  Everything below it
is invisible unless exposed through a ReControl command.

---

## 2. What's Exposed via ReControl/MCP

These 28 commands are the AI developer's complete toolkit:

| Category | Commands |
|----------|----------|
| State | `state`, `info`, `apps`, `dbs` |
| Input | `tap`, `tap-id`, `pen`, `key`, `type`, `button` |
| Screen | `screenshot`, `screen-hash`, `ui` |
| Session | `install`, `export`, `launch`, `save`, `load`, `reset`, `sleep`, `quit` |
| Dialog | `dialog` (query + respond, includes regs in `blocked_on_ui`) |
| Memory | `peek`, `poke`, `regs` |
| Menu | `menu` |
| Database | `delete` |
| Batch | `run` |

**Recent improvements (this session):**
- `dialog` query now includes CPU register dump in `blocked_on_ui` state
- `reset` works in `blocked_on_ui` (dismisses dialog, unblocks CPU)
- `regs` and `peek` work in `blocked_on_ui` state
- `load` works from cold start (no existing session required)
- `install` timeout scales with file size (5s + ~10s/MB)
- Error messages include file context and recovery suggestions

---

## 3. What's Exposed via Qt GUI Only

These features have working Qt dialogs but NO ReControl command:

| Feature | Menu Path | Dialog | What It Controls |
|---------|-----------|--------|-----------------|
| **Debugging Options** | Settings > Debugging | 18 memory-violation checkboxes | Report flags for free chunk, low memory, ROM access, etc. |
| **Logging Options** | Settings > Logging | 20 logging-category checkboxes | CPU opcodes, syscalls, events, serial, network, etc. |
| **Error Handling** | Settings > Error Handling | 4 dropdowns | Warning/Error behavior: Show/Continue/Quit/Switch |
| **Gremlins** | Test > New Gremlin | Gremlin configuration dialog | Seed, event limit, app list |

An AI developer using MCP cannot access any of these.

---

## 4. What Exists in Code but Is Inaccessible

These features are fully implemented, compiled, and functional — but have
neither a ReControl command NOR a working Qt dialog:

| Feature | Implementation | Lines of Code | Status |
|---------|---------------|---------------|--------|
| **Breakpoints** | `DebugMgr.h/.cpp` | ~2000 | Code complete, no Qt dialog, no ReControl |
| **Watchpoints** | `DebugMgr.h` (watchEnabled/watchAddr) | ~200 | Code complete, no UI at all |
| **Step Spy** | `DebugMgr.h` (stepSpy/ssAddr) | ~100 | Code complete, no UI at all |
| **Stack Crawl** | `EmPalmOS::GenerateStackCrawl()` | ~70 | Complete, used internally by error handler |
| **Memory Diagnostics** | `MetaMemory.h/.cpp` | ~3000 | Complete, controlled by pref flags only |

---

## 5. What's Compiled Out

| Feature | Preprocessor Guard | Current Value | Location |
|---------|-------------------|---------------|----------|
| **Profiling** | `HAS_PROFILING` | `0` | `CMakeLists.txt:46` |
| **Tracer** | `HAS_TRACER` | `0` (non-Mac) | `Switches.h:188-194` |

Profiling is the biggest loss.  The code is ~1500 lines of cycle-accurate
function-level profiling with call tree analysis and Metrowerks-compatible
output.  It's all there, just guarded by `#if HAS_PROFILING`.

---

## 6. What Was Never Ported from FLTK

The original FLTK-based POSE had these UI elements that don't exist in Qt:

| Dialog | Original Location | Qt Status |
|--------|-------------------|-----------|
| **Breakpoint Manager** | `EmDlgFltkFactory.fl` | Not ported. `kDlgEditBreakpoints` falls through to `kDlgItemNone` |
| **Code Breakpoint Editor** | `EmDlgFltkFactory.fl` | Not ported |
| **Data Breakpoint Editor** | `EmDlgFltkFactory.fl` | Not ported |
| **Trap Break Manager** | `EmDlgFltkFactory.fl` | Not ported |
| **Profile Controls** | Menu items | Commands stub to `DoNothing()` |
| **State Inspector** | Various | Not ported |
| **Comprehensive Debug Options** | Full error/logging config | Qt version only has 18 report-flag checkboxes (vs full original) |

---

## 7. Feature Details

### 7.1 Breakpoint System

**Files:** `src/core/DebugMgr.h`, `src/core/DebugMgr.cpp`

POSE supports 6 instruction breakpoints (5 normal + 1 temporary) and 5 trap
breakpoints, each with optional conditions.

**Breakpoint structure:**
```cpp
struct EmBreakpointType {
    MemPtr   addr;      // address to break at
    Boolean  enabled;   // true if active
    Boolean  installed; // runtime state
};
```

**Conditional expressions** use a mini-language:
```
d5.w == 0x1234       // break when D5 word equals 0x1234
8(a6).l != 0         // break when long at A6+8 is non-zero
a0.l >= 0x10000      // break when A0 >= 64K
```

**Condition structure:**
```cpp
struct BreakpointCondition {
    registerFun  regType;        // register function (D0-D7, A0-A7)
    int          regNum;         // register number
    Bool         indirect;       // dereference register as pointer?
    uint32       indirectOffset; // offset from register value
    int          size;           // 1, 2, or 4 bytes to compare
    compareFun   condition;      // ==, !=, <, >, <=, >=
    uint32       value;          // value to compare against
    char*        source;         // original expression string
    Bool Evaluate(void);         // evaluate condition against current CPU state
};
```

**Key API:**
```cpp
Debug::SetBreakpoint(int index, emuptr addr, BreakpointCondition* c);
Debug::ClearBreakpoint(int index);
Debug::NewBreakpointCondition(const char* sourceString);  // parse "d5.w == 0x1234"
Debug::DeleteBreakpointCondition(int index);
Debug::BreakpointInstalled();  // any breakpoints active?
Debug::HandleInstructionBreak();  // called when BP hit
Debug::InstallInstructionBreaks();  // write BP opcodes into emulated memory
Debug::RemoveInstructionBreaks();   // restore original opcodes
```

**What it would take to expose via ReControl:**
New commands: `break set <addr> [condition]`, `break clear <index>`,
`break list`, `break enable/disable <index>`.  All the underlying code
works.  Just needs command parsing and response formatting.

---

### 7.2 Watchpoints and Step Spy

**File:** `src/core/DebugMgr.h` (fields in `DebugGlobalsType`)

**Watchpoints** monitor a memory range for writes:
```cpp
gDebuggerGlobals.watchEnabled = true;
gDebuggerGlobals.watchAddr = 0x12340;  // start address
gDebuggerGlobals.watchBytes = 16;       // number of bytes
```

When any byte in the range is written, `Debug::DoCheckWatchpoint()` fires
and `Errors::ReportErrWatchpoint()` shows a dialog with the write address,
old value, and new value.

**Step Spy** monitors a single address for modification:
```cpp
gDebuggerGlobals.stepSpy = true;
gDebuggerGlobals.ssAddr = 0x12340;     // address to watch
gDebuggerGlobals.ssValue = oldValue;    // saved original value
```

`Debug::CheckStepSpy()` is called inline from every memory write operation
for maximum performance.  When the value changes,
`Errors::ReportErrStepSpy()` fires.

**What it would take to expose via ReControl:**
New commands: `watch set <addr> <nbytes>`, `watch clear`, `spy set <addr>`,
`spy clear`.  Trivial — just set the globals and enable.

---

### 7.3 Stack Crawl / Backtrace

**File:** `src/core/EmPalmOS.cpp` (lines 860-928)

```cpp
void EmPalmOS::GenerateStackCrawl(EmStackFrameList& frameList);
```

Walks the A6 (frame pointer) chain to produce a call stack:

```cpp
struct EmStackFrame {
    emuptr fAddressInFunction;  // return address (code location in caller)
    emuptr fA6;                 // frame pointer for this frame
};
typedef std::vector<EmStackFrame> EmStackFrameList;
```

**Algorithm:**
1. Start with current PC and A6
2. Read saved A6 at `[A6+0]`, return address at `[A6+4]`
3. Validate: A6 must be even, within stack bounds, and increasing
4. Handle exception frames (return address at `[A6+6]`)
5. Stop when validation fails

**Used internally by:**
- Error handler (stack trace in crash dialogs)
- Memory leak detector (allocation site tracking)
- Event logger (call context)

**What it would take to expose via ReControl:**
New command: `backtrace` or `bt`.  Call `GenerateStackCrawl()`, format as
multi-line response with addresses.  Works in `blocked_on_ui` since CPU is
frozen.  Maybe 20 lines of code.

---

### 7.4 Profiling

**Files:** `src/core/Profiling.h`, `src/core/Profiling.cpp`

**Currently disabled:** `HAS_PROFILING=0` in `CMakeLists.txt:46`

This is a cycle-accurate CPU profiler with function-level timing:

**Capabilities:**
- Clock cycle counting per instruction
- Memory access cycle counting (read/write separately, with wait states)
- Function entry/exit tracking with call tree
- Interrupt profiling
- Instruction-level detailed profiling for specific address ranges
- Output in Metrowerks Profile (`.mwp`) and text (`.txt`) formats

**Constants:**
```cpp
#define MAXFNCALLS      0x10000   // 65,536 function calls tracked
#define MAXUNIQUEFNS    2500      // unique function addresses
#define MAXDEPTH        200       // call tree depth
```

**API:**
```cpp
ProfileInit(int maxCalls, int maxDepth);  // allocate profiler
ProfileStart();                            // begin collecting
ProfileStop();                             // pause collecting
ProfileDump(const char* filename);         // write .mwp file
ProfilePrint(const char* filename);        // write .txt report
ProfileCleanup();                          // free profiler
ProfileDetailFn(emuptr addr, int logInstructions);  // detailed tracing
```

**Global counters:**
```cpp
extern int64 gClockCycles;   // total CPU cycles
extern int64 gReadCycles;    // memory read cycles
extern int64 gWriteCycles;   // memory write cycles
```

**Wait state modeling:**
```cpp
#define WAITSTATES_ROM       2
#define WAITSTATES_SRAM      4
#define WAITSTATES_DRAM      4
#define WAITSTATES_REGISTERS 0
```

**Host Control selectors** (callable from Palm OS app code):
```
hostSelectorProfileInit       0x0200
hostSelectorProfileStart      0x0201
hostSelectorProfileStop       0x0202
hostSelectorProfileDump       0x0203
hostSelectorProfileCleanup    0x0204
hostSelectorProfileDetailFn   0x0205
hostSelectorProfileGetCycles  0x0206
```

**What it would take to expose via ReControl:**
1. Set `HAS_PROFILING=1` in CMakeLists.txt
2. New commands: `profile init`, `profile start`, `profile stop`,
   `profile dump <path>`, `profile cleanup`, `profile cycles`
3. The EmDocument handlers (`DoProfileStart` etc.) already exist and work

---

### 7.5 Logging

**Files:** `src/core/Logging.h`, `src/core/Logging.cpp`

**20 logging categories** (each independently toggleable):

| Category | What It Logs |
|----------|-------------|
| `LogErrorMessages` | Error conditions |
| `LogWarningMessages` | Warnings |
| `LogGremlins` | Gremlin events |
| `LogCPUOpcodes` | Every m68k instruction executed |
| `LogEnqueuedEvents` | Events added to Palm OS event queue |
| `LogDequeuedEvents` | Events consumed from Palm OS event queue |
| `LogSystemCalls` | Palm OS trap calls (system API) |
| `LogApplicationCalls` | ROM application function calls |
| `LogSerial` | Serial port activity |
| `LogSerialData` | Serial port data bytes |
| `LogNetLib` | Network library calls |
| `LogNetLibData` | Network data bytes |
| `LogExgMgr` | Exchange Manager activity |
| `LogExgMgrData` | Exchange Manager data |
| `LogHLDebugger` | High-level debugger protocol |
| `LogHLDebuggerData` | High-level debugger data |
| `LogLLDebugger` | Low-level debugger protocol |
| `LogLLDebuggerData` | Low-level debugger data |
| `LogRPC` | RPC calls |
| `LogRPCData` | RPC data |

**Preference values:** `0` = off, `1` = Gremlin mode only, `2` = always on.

**Log file configuration:**
- `kPrefKeyLogFileSize` — max size, default 1MB
- `kPrefKeyLogDefaultDir` — output directory

**LogStream class:**
```cpp
LogStream::Printf(const char* fmt, ...);      // timestamped output
LogStream::DataPrintf(void*, long, fmt, ...);  // hex dump + message
LogStream::Clear();                             // clear buffer
LogStream::DumpToFile();                        // flush to disk
LogStream::EnsureNewFile();                     // start new log file
```

**Qt dialog status:** WORKING.  `Settings > Logging` shows all 20
categories as checkboxes with Normal/Gremlin toggle.

**What it would take to expose via ReControl:**
New commands: `log list` (show categories + current state),
`log set <category> <0|1|2>`, `log dump <path>`, `log clear`.
All the infrastructure works.

---

### 7.6 Memory Diagnostics (MetaMemory)

**Files:** `src/core/MetaMemory.h`, `src/core/MetaMemory.cpp`

Every byte of emulated memory has 8 bits of metadata tracking what kind of
memory region it belongs to.  When application code accesses a region
inappropriately, the emulator catches it.

**Region types tracked:**
- Low memory (interrupt vectors, globals)
- System globals
- Heap headers and chunk headers/trailers
- Memory pool table (MPT)
- Low stack (overflow zone)
- Unlocked handle data
- Free chunk data
- Screen buffer
- UI form/control objects

**18 report-on-violation flags** (each independently toggleable):

| Flag | What It Reports |
|------|----------------|
| `ReportFreeChunkAccess` | Read/write to freed memory |
| `ReportHardwareRegisterAccess` | Direct hardware register access |
| `ReportLowMemoryAccess` | Access to interrupt vectors / trap table |
| `ReportLowStackAccess` | Stack pointer entering danger zone |
| `ReportMemMgrDataAccess` | Access to memory manager internals |
| `ReportMemMgrLeaks` | Memory leaks on app exit |
| `ReportMemMgrSemaphore` | Memory manager semaphore violations |
| `ReportOffscreenObject` | UI object drawn outside form bounds |
| `ReportOverlayErrors` | Overlay segment errors |
| `ReportProscribedFunction` | Calling deprecated/forbidden APIs |
| `ReportROMAccess` | Writing to ROM |
| `ReportScreenAccess` | Direct screen buffer access (bypassing WinAPI) |
| `ReportSizelessObject` | Zero-size UI objects |
| `ReportStackAlmostOverflow` | Stack within N bytes of limit |
| `ReportStrictIntlChecks` | International text handling violations |
| `ReportSystemGlobalAccess` | Direct system global access |
| `ReportUIMgrDataAccess` | Direct UI manager structure access |
| `ReportUnlockedChunkAccess` | Access to unlocked handle data |

**All default to OFF** in the Qt port (they were off on 64-bit platforms
historically).

**Qt dialog status:** WORKING.  `Settings > Debugging` shows all 18 flags.

**What it would take to expose via ReControl:**
New commands: `check list` (show flags + state), `check set <flag> <on|off>`,
`check set-all <on|off>`.

---

### 7.7 Error Handling Configuration

**Files:** `src/core/ErrorHandling.h`, `src/core/ErrorHandling.cpp`

**Four behavior settings:**

| Setting | What It Controls | Default |
|---------|-----------------|---------|
| `WarningOff` | Behavior for warnings when Gremlins OFF | Show |
| `ErrorOff` | Behavior for errors when Gremlins OFF | Show |
| `WarningOn` | Behavior for warnings when Gremlins ON | Show |
| `ErrorOn` | Behavior for errors when Gremlins ON | Show |

**Options for each:** Show dialog, Continue silently, Quit, Switch to debugger.

**Error types detected (50+):**

*Hardware exceptions:*
Bus Error, Address Error, Illegal Instruction, Divide by Zero, CHK, TRAPV,
Privilege Violation, Trace, A-Trap, F-Trap, TRAP #x

*Fatal errors:*
Stack Overflow, Unimplemented Trap, Invalid RefNum, Corrupted Heap, Invalid PC

*Non-fatal memory violations:*
(controlled by the 18 MetaMemory flags above)

*Palm OS errors:*
SysFatalAlert interception, DbgMessage interception

**Qt dialog status:** WORKING.  `Settings > Error Handling` shows 4 dropdowns.

**What it would take to expose via ReControl:**
New command: `errorhandling get`, `errorhandling set <setting> <show|continue|quit|debug>`.

---

### 7.8 Gremlins / Hordes

**Files:** `src/core/CGremlins.h`, `src/core/Hordes.h`

Gremlins inject random events (pen taps, key presses, app switches) into the
emulator for automated stress testing.  Hordes coordinate multiple Gremlin
runs.

**Gremlin capabilities:**
- Configurable random seed (for reproducibility)
- Event limit (run N events then stop)
- Save/resume state (checkpoint at any point)
- 2048-character keyboard input buffer
- Pen movement tracking (random but physically plausible)
- Start/stop timing

**GremlinInfo structure:**
```cpp
struct GremlinInfo {
    int32            fNumber;         // seed number
    int32            fSteps;          // steps to run (-1 = infinite)
    int32            fFinal;          // final step number
    int32            fSaveFrequency;  // save state every N events (default: 10000)
    DatabaseInfoList fAppList;        // apps to test
};
```

**Hordes control:**
```cpp
Hordes::New(const HordeInfo& info);    // start batch of Gremlins
Hordes::NewGremlin(const GremlinInfo&); // single Gremlin
Hordes::Suspend();                      // pause
Hordes::Step();                         // single event
Hordes::Resume();                       // continue
Hordes::Stop();                         // terminate
Hordes::Status(&number, &step, &until); // query progress
Hordes::SaveSearchProgress();           // checkpoint
Hordes::ErrorEncountered();             // log error
```

**Event types generated:**
1. Random pen events (tap, drag, lift)
2. Random key events (printable characters)
3. Random app switches

**Qt dialog status:** WORKING.  `Test > New Gremlin` and Gremlin control
dialog (suspend/step/resume/stop) are functional.

**Host Control selectors** (callable from Palm OS code):
```
hostSelectorGremlinIsRunning  0x0400
hostSelectorGremlinNumber     0x0401
hostSelectorGremlinCounter    0x0402
hostSelectorGremlinLimit      0x0403
hostSelectorGremlinNew        0x0404
```

**What it would take to expose via ReControl:**
New commands: `gremlin new <seed> <events> [apps...]`, `gremlin status`,
`gremlin suspend`, `gremlin step`, `gremlin resume`, `gremlin stop`.
All the code works.  The EmDocument handlers exist.

---

### 7.9 Tracer

**Files:** `src/core/TracerCommon.h`, `src/core/TracerPlatform.h`

**Currently disabled:** `HAS_TRACER=0` on non-Mac platforms (`Switches.h:188-194`)

Instruction-level tracing to external output with configurable tracer types.

**TracerBase interface:**
```cpp
virtual void Initialize();
virtual void Dispose();
virtual void InitOutputPort();
virtual void CloseOutputPort();
virtual void OutputVT(errModule, formatString, args);   // text output
virtual void OutputVTL(errModule, formatString, args);  // text + newline
virtual void OutputB(errModule, buffer, bufferLen);      // binary output
virtual long GetConnectionStatus();
virtual void GetTracerCapabilities(buffer, bufferLen);
```

**Host Control selectors:**
```
hostSelectorTraceInit      0x0900
hostSelectorTraceClose     0x0901
hostSelectorTraceOutputT   0x0902
hostSelectorTraceOutputTL  0x0903
hostSelectorTraceOutputVT  0x0904
hostSelectorTraceOutputVTL 0x0905
hostSelectorTraceOutputB   0x0906
```

**Status:** Would require `HAS_TRACER=1` and platform-specific implementation
for Linux/Qt.

---

### 7.10 CPU State and History

**File:** `src/core/Hardware/EmCPU68K.h`

**Always available:**
- All m68k registers: D0-D7, A0-A7, PC, SR/CCR
- Read via `m68k_dreg(regs, N)`, `m68k_areg(regs, N)`, `regs.pc`, `regs.sr`
- Write via same macros (l-values)
- Currently exposed via `regs` command

**Debug-build only (REGISTER_HISTORY / EXCEPTION_HISTORY):**
- Last 512 register states
- Last 512 exception numbers
- Controlled by `ON_IN_DEBUG_MODE` macro

**Exception handling:**
All 68K exceptions are intercepted:
```cpp
kException_BusErr           = 2
kException_AddressErr       = 3
kException_IllegalInstr     = 4
kException_DivideByZero     = 5
kException_CHK              = 6
kException_TRAPV            = 7
kException_PrivilegeViolation = 8
kException_Trace            = 9   // single-step
kException_ATrap            = 10  // A-line (Palm OS traps)
kException_FTrap            = 11  // F-line
kException_SoftBreak        = TRAP0 + sysDbgBreakpointTrapNum
kException_HardBreak        = TRAP0 + sysDbgTrapNum
```

---

### 7.11 ROM Function Calling

**Files:** `src/core/ROMStubs.h`, `src/core/EmSubroutine.h`

100+ Palm OS functions callable from emulator code:

**Database:** DmCreateDatabase, DmOpenDatabase, DmCloseDatabase,
DmFindDatabase, DmDatabaseInfo, DmGetNextDatabaseByTypeCreator,
DmNewResource, DmGetResource, etc.

**Events:** EvtAddEventToQueue, EvtEnqueueKey, EvtEnqueuePenPoint,
EvtWakeup, EvtGetEvent, EvtGetPen, etc.

**Forms:** FrmGetActiveForm, FrmGetNumberOfObjects, FrmGetObjectId,
FrmGetObjectBounds, FrmGetObjectType, etc.

**Fields:** FldGetTextPtr, FldGetTextLength, FldGetInsPtPosition, etc.

**Memory:** MemPtrNew, MemHandleLock, MemHandleUnlock, MemPtrFree,
MemHeapCheck, etc.

**System:** SysUIAppSwitch, SysAppLaunch, SysReset, SysTaskDelay, etc.

**Mechanism:**
```cpp
EmSubroutine sub;
sub.AddParam("param1", value1);
sub.AddParam("param2", value2);
sub.Call(sysTrapXxx);    // execute trap
sub.GetReturnVal(result);
```

Via `ExecuteSubroutine`, which saves CPU state, sets up the call frame,
runs the ROM function, and restores state.

---

### 7.12 Session State Debugging

**File:** `src/core/SessionFile.h`

Session files (`.psf`) save and restore ALL debugging state:

| Chunk Tag | What It Contains |
|-----------|-----------------|
| `kCPURegs` | All m68k registers |
| `kDBState` / `kDBEZState` / `kDBVZState` | Hardware registers (Dragonball variants) |
| `zram` | Compressed RAM image |
| `zmrm` / `zmro` | Meta-RAM and Meta-ROM (memory protection state) |
| `kDebugInfo` | Breakpoints, watchpoints, debugger state |
| `kGremlinInfo` | Current Gremlin configuration |
| `kGremlinHistory` | Compressed event history |
| `kProfileInfo` | Profiling data |
| `kLoggingInfo` | Logging preferences |
| `kMetaInfo` | Memory metadata state |
| `kPatchInfo` | ROM patch state |
| `kStackInfo` | Stack analysis data |
| `kHeapInfo` | Heap analysis data |

This means: breakpoints, watchpoints, profiling state, and Gremlin progress
survive session save/load.  A developer can set breakpoints, save the
session, and resume debugging later.

---

## 8. Status Matrix

| Feature | Code | Compiled | Qt GUI | ReControl | MCP |
|---------|------|----------|--------|-----------|-----|
| Register dump | Complete | Yes | — | `regs` | `palm_regs` |
| Memory read/write | Complete | Yes | — | `peek`/`poke` | `palm_peek`/`palm_poke` |
| Stack crawl | Complete | Yes | Error dialogs only | **NONE** | **NONE** |
| Breakpoints (6) | Complete | Yes | **NOT PORTED** | **NONE** | **NONE** |
| Watchpoints | Complete | Yes | **NOT PORTED** | **NONE** | **NONE** |
| Step Spy | Complete | Yes | **NOT PORTED** | **NONE** | **NONE** |
| Profiling | Complete | **NO** (`HAS_PROFILING=0`) | **NOT PORTED** | **NONE** | **NONE** |
| Logging (25 cats) | Complete | Yes | Working | **NONE** | **NONE** |
| MetaMemory (18 flags) | Complete | Yes | Working | **NONE** | **NONE** |
| Error handling config | Complete | Yes | Working | **NONE** | **NONE** |
| Gremlins/Hordes | Complete | Yes | Working | **NONE** | **NONE** |
| Tracer | Complete | **NO** (`HAS_TRACER=0`) | **NOT PORTED** | **NONE** | **NONE** |
| Session save/load | Complete | Yes | Working | `save`/`load` | `palm_save`/`palm_load` |
| Dialog dismiss | Complete | Yes | Working | `dialog respond` | `palm_dialog` |
| Reset in blocked | Complete | Yes | Working | `reset` | `palm_reset` |

---

## 9. Priority Recommendations

### Must Expose (highest debugging value, lowest implementation cost)

1. **`backtrace`** — Call `EmPalmOS::GenerateStackCrawl()`, return frame list.
   ~20 lines.  Makes crash debugging actually possible.

2. **`break` commands** — Set/clear/list breakpoints.  All code exists.
   ~100 lines of ReControl command parsing.

3. **`watch` / `spy` commands** — Set/clear watchpoints and step spy.
   ~50 lines.  Trivially maps to DebugGlobals fields.

4. **`log` commands** — Control logging categories, dump log to file.
   ~80 lines.  Infrastructure is 100% working.

### Should Expose (high value, moderate cost)

5. **`gremlin` commands** — Start/stop/status for automated stress testing.
   ~100 lines.  Handlers exist in EmDocument.

6. **`check` commands** — Toggle MetaMemory report flags.
   ~60 lines.  Maps directly to preference keys.

7. **`errorhandling` commands** — Configure error behavior.
   ~40 lines.  Maps to 4 preference values.

### Requires Build Change + Exposure

8. **`profile` commands** — Enable `HAS_PROFILING=1`, add ReControl commands.
   Build change is one line.  Command handlers exist in EmDocument.
   ~60 lines of ReControl.

### Nice to Have (high cost, niche value)

9. **Tracer** — Requires `HAS_TRACER=1` plus Linux implementation.
   Original was Mac-only.

10. **CPU History** — Requires debug build.  Useful for post-mortem analysis.

---

## File Reference

| File | Purpose |
|------|---------|
| `src/core/DebugMgr.h` | Breakpoints, watchpoints, step spy, debugger protocol |
| `src/core/DebugMgr.cpp` | Implementation (~2000 lines) |
| `src/core/Profiling.h` | Profiler API and globals |
| `src/core/Profiling.cpp` | Profiler implementation (~1500 lines) |
| `src/core/Logging.h` | Logging categories and LogStream |
| `src/core/Logging.cpp` | Logging implementation |
| `src/core/MetaMemory.h` | Memory access checking API |
| `src/core/MetaMemory.cpp` | MetaMemory implementation (~3000 lines) |
| `src/core/ErrorHandling.h` | Error reporting and handling API |
| `src/core/ErrorHandling.cpp` | Error handler implementation |
| `src/core/Hordes.h` | Gremlin/Hordes API |
| `src/core/CGremlins.h` | Gremlin event generation |
| `src/core/EmPalmOS.cpp:860` | `GenerateStackCrawl()` |
| `src/core/TracerCommon.h` | Tracer abstract interface |
| `src/core/ROMStubs.h` | 100+ callable ROM functions |
| `src/core/EmSubroutine.h` | ROM function call mechanism |
| `src/core/SessionFile.h` | Session state chunk definitions |
| `src/core/PreferenceMgr.h` | All debug-related preference keys |
| `src/core/EmCommands.h` | Command IDs for debug features |
| `src/core/EmApplication.cpp` | Command dispatch (some stub to DoNothing) |
| `src/core/EmDocument.cpp` | Gremlin/Profile handlers |
| `src/platform/EmDlgQt.cpp` | Qt dialog implementations (some missing) |
| `src/core/Switches.h` | HAS_TRACER, HAS_OMNI_THREAD |
| `CMakeLists.txt:46` | HAS_PROFILING=0 |
