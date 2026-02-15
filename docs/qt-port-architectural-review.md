# QtPortPOSE vs RePOSE4 (FLTK): Architectural Review

## Architecture Overview: What Changed

The FLTK original has a **two-thread model**: the main thread runs `Fl::wait()` + `HandleIdle()` (event loop AND emulator idle pump on the same thread), while a CPU thread runs m68k instructions. Simple, battle-tested.

The Qt port introduced a **three-thread model**: Qt UI thread (event loop only), a "bridge" thread (runs HandleIdle via `PoseIdleWorker`), and the CPU thread. This was done because Qt's event loop must never block, and `HandleIdle()` can block on `EmSessionStopper`. The concept is sound. The execution is where things went off the rails.

---

## The Good

### 1. Bridge thread concept is correct
Moving `HandleIdle()` off the UI thread was the right call. FLTK could get away with blocking `Fl::wait()` because FLTK is simple. Qt cannot. The `PoseIdleWorker` on a QThread with a single-shot timer is the right pattern.

### 2. Cross-thread screen delivery is well-done
`EmWindowQt.cpp:128-134` — Using `QMetaObject::invokeMethod` with `Qt::QueuedConnection` and copying QImages by value is textbook Qt thread-safety. The framebuffer is converted to QImage on the bridge thread (`emPixMapToQImage`), then posted to the UI thread for painting. No shared mutable state.

### 3. Atomic mouse position is clean
`EmWindowQt.h:91-92` — `QAtomicInt fMouseX/fMouseY` lets the bridge thread read mouse coordinates (via `HostGetCurrentMouse`) without touching QWidget state. Simple, correct.

### 4. The omnithread shim is excellent
`qt-pose/src/core/omnithread/omnithread.h` — A complete, API-compatible replacement of the CORBA omnithread library using QMutex, QWaitCondition, and QThread. Handles the absolute-to-relative time conversion for `timedwait()`, implements RAII locks and unlocks, semaphores. 227 lines that saved touching ~18 core files. This is the best piece of engineering in the port.

### 5. emPixMapToQImage handles all formats
`EmWindowQt.cpp:177-280` — All 13+ EmPixMap formats (1-bit mono, 8-bit indexed, 16-bit RGB555, 24-bit RGB, 32-bit ARGB/RGBA) are correctly converted. The fallback path uses `ConvertToFormat` for exotic types. Deep copies transient data. Thorough.

### 6. New Session dialog is well-implemented
`EmDlgUnix.cpp:151-325` — Proper Qt dialog with ROM MRU dropdown, device filtering based on ROM compatibility, RAM size filtering based on device minimums, browse button, signal/slot wiring. This is how Qt dialogs should be built.

### 7. Build system is solid
`CMakeLists.txt` — Clean Qt6 CMake integration. Correctly excludes conflicting sources (bundled Gzip vs system zlib, UAE build tools, Palm SDK implementation files, omnithread platform files). Source categories are well-organized.

---

## The Bad

### 1. `EmApplication::Startup()` is NEVER called

This is the single most consequential error. Compare:

**FLTK** (`EmApplicationFltk.cpp:101`):
```cpp
theApp.Startup(argc, argv)  // calls EmApplication::Startup()
```

**Qt** (`EmBridge.cpp:52`):
```cpp
sApplication = new EmApplication;  // just the constructor!
sPreferences->Load();              // manual partial init
```

`EmApplication::Startup()` (`EmApplication.cpp:189-234`) does:
- `gPrefs->Load()` — duplicated in bridge, OK
- `CSocket::Startup()` — **SKIPPED** — no socket infrastructure
- `Debug::Startup()` — **SKIPPED** — no debugger support
- `RPC::Startup()` — **SKIPPED** — no RPC support
- `LogStartup()` — **SKIPPED** — no logging subsystem
- `Startup::DetermineStartupActions()` — **SKIPPED** — command-line args ignored

The bridge tries to replicate two lines out of dozens. The entire subsystem initialization chain is missing.

### 2. No `EmApplicationFltk` equivalent — no platform subclass at all

In FLTK, `EmApplicationFltk` subclasses `EmApplication` and overrides `HandleIdle()`:

```cpp
// EmApplicationFltk.cpp:283-296
void EmApplicationFltk::HandleIdle(void) {
    if (!this->PrvIdleClipboard())  // clipboard sync
        return;
    ::HandleDialogs();              // modeless dialog pump
    EmApplication::HandleIdle();    // base idle (CPU + screen)
}
```

The Qt port uses the **bare `EmApplication` base class** with no subclass. There is no `EmApplicationQt`. This means:
- **Clipboard handling is completely absent** — the entire clipboard sync system (outgoing selections, incoming paste events) is gone
- **`HandleDialogs()` is never called** — modeless dialogs (debug windows, Gremlin status) silently fail
- The `POSEApplication` QApplication subclass exists but is just a thin wrapper around `PoseBridge::initialize()` — it doesn't participate in POSE's architecture at all

### 3. Window ownership model is inverted

**FLTK** (`EmApplicationFltk.cpp:312-319` + `EmWindowFltk.cpp:52-63`):
```cpp
// Application creates the window during Startup
void EmApplicationFltk::PrvCreateWindow(...) {
    fAppWindow = new EmWindowFltk;
    fAppWindow->WindowInit();
    fAppWindow->show(argc, argv);
}
// NewWindow() returns NULL — window already exists
EmWindow* EmWindow::NewWindow() { return NULL; }
```

**Qt** (`main.cpp:36-41` + `EmWindowUnix.cpp:10-13`):
```cpp
// main() creates the window independently
EmWindow* window = EmWindow::NewWindow();  // returns new EmWindowQt
window->WindowInit();
// NewWindow() actually creates — but who owns it?
```

In FLTK, the application owns its window through `fAppWindow`. In Qt, `main()` creates the window, nobody stores the pointer (it goes into `gWindow` via the base constructor), and there's no `fAppWindow` member anywhere. The window is an orphan managed by a global.

### 4. Zero keyboard input handling

**FLTK** (`EmWindowFltk.cpp:246-327`): Comprehensive key mapping — printable chars go to `HandleKey()`, F1-F4 map to app buttons, PageUp/Down to scroll buttons, F9 to power, Enter to line feed, arrow keys to navigation.

**Qt** (`EmWindowQt.cpp:315-350`): Only mouse events. No `keyPressEvent()` override at all. The emulated Palm has no physical buttons and no keyboard input.

### 5. No ReControl socket API

The FLTK version's `ReControl.cpp` (800+ lines) provides the Unix socket control interface — install PRCs, take screenshots, inject pen/button events, read memory, launch apps. This is critical for automated testing (per CLAUDE.md's workflow). The Qt port has zero ReControl integration — not even stubs.

### 6. HostRectFrame and HostOvalPaint are visual no-ops

**FLTK** (`EmWindowFltk.cpp:561-587`): Actually draws button-press feedback rectangles and LED indicator ovals with `fl_rect()` and `fl_pie()`.

**Qt** (`EmWindowQt.cpp:502-516`): Both just call `update()` — triggering a repaint of the existing skin/LCD images. No rectangles or ovals are actually drawn. Button press feedback and LED indicators are silently invisible.

### 7. `EmApplication::Shutdown()` is never called

`PoseBridge::shutdown()` (`EmBridge.cpp:67-85`) destroys the session thread and closes the document, but never calls `EmApplication::Shutdown()`. This means:
- `gPrefs->Save()` is never called — preferences are never persisted
- `Debug::Shutdown()`, `RPC::Shutdown()`, `CSocket::Shutdown()` — never cleaned up
- `LogShutdown()` — never called
- `EmTransport::CloseAllTransports()` (in `~EmApplication`) — may or may not run depending on destruction order

---

## The Ugly

### 1. 64-bit pointer truncation — the fundamental unsoundness

`CMakeLists.txt:71-76`:
```cmake
$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
$<$<COMPILE_LANGUAGE:CXX>:-Wno-int-to-pointer-cast>
$<$<COMPILE_LANGUAGE:CXX>:-Wno-pointer-to-int-cast>
```

The comments even say "These are BROKEN and need proper fixes." The FLTK version was 32-bit (`-m32`), where `sizeof(void*) == sizeof(int) == 4`. In the Qt 64-bit build, every `emuptr` (uint32) to host pointer cast silently truncates the upper 32 bits. This isn't a warning to fix later — it's active, silent data corruption happening right now on every affected code path. The `-fpermissive` flag turns what should be hard errors into silent poison.

### 2. `omni_thread::self()` returns nullptr — breaks CPU thread identity

`omnithread.h:176`:
```cpp
static omni_thread* self() { return 0; }
```

The comment admits: "InCPUThread() always returns false." `EmSession::InCPUThread()` compares `omni_thread::self()` against `fThread` to determine if the caller is the CPU thread. Always returning false means the CPU thread **thinks it's the UI thread**. Any code that uses `InCPUThread()` to decide whether locking is needed will make the wrong decision. This is a latent data race affecting the entire synchronization model.

### 3. The three-thread model creates deadlock potential that didn't exist before

FLTK had two threads. The synchronization protocol was simple: UI thread calls `EmSessionStopper` to pause CPU, does work, releases. One direction, one lock.

Qt has three threads, but `HandleCommand` is still called from the UI thread (`EmWindowQt.cpp:377`):
```cpp
gApplication->HandleCommand(cmd);  // UI thread!
```

Many `HandleCommand` handlers use `EmSessionStopper` internally (Save, Reset, Export). So:
- Bridge thread may be blocked on `EmSessionStopper` (via `HandleIdle()`)
- UI thread also calls `EmSessionStopper` (via menu commands)
- Both are waiting for the CPU thread to reach a syscall boundary
- If both request suspension simultaneously, the ordering of condition variable wakeups could cause starvation or deadlock depending on timing

The FLTK version never had this problem because HandleIdle and HandleCommand ran on the same thread — they could never overlap.

### 4. `stopBridgeThread` has a race condition with double-quit

`EmWindowQt.cpp:96-120`:
```cpp
QMetaObject::invokeMethod(fWorker, "stop", Qt::QueuedConnection);  // queues stop
fWorkerThread->quit();  // also quits the thread's event loop
```

`PoseIdleWorker::stop()` also calls `QThread::currentThread()->quit()`. So the thread gets two quit signals racing. If `quit()` arrives before the queued `stop()`, the worker's timer is never cleaned up. If `stop()` arrives first, the second `quit()` is a no-op but only by luck.

### 5. The initialization sequence is an inside-out version of the original

The FLTK lifecycle is clean and linear:
```
main() -> EmulatorPreferences -> EmApplicationFltk -> Startup() ->
  CreateWindow() -> MenuInitialize() -> ClipboardInit -> Run() -> Shutdown()
```

The Qt version scatters this across four different classes with unclear ownership:
```
main() -> POSEApplication -> PoseBridge::initialize() [partial Startup]
  -> main() calls MenuInitialize() [not in bridge]
  -> main() calls NewWindow() [not in application]
  -> main() calls HandleStartupActions() [originally inside Run()]
  -> main() calls HandleNewFromPrefs() [new code, not in FLTK]
  -> app.exec() -> HostWindowShow() starts bridge thread [in window, not app]
```

There is no single authority over the startup sequence. The bridge does some init, main() does more, the window starts the idle thread. If any step fails or happens in the wrong order, the state is inconsistent with no error handling.

### 6. Half the dialog system is TODO stubs

`EmDlgUnix.cpp:347-643` — After the well-implemented New Session dialog, there are **25 consecutive TODO stub functions**. `HostDialogOpen`, `HostDialogClose`, `HostStartIdling`, `SetItemText`, `EnableItem`, `DisableItem`, `AppendToMenu`, `AppendToList`, `SelectListItems`, `GetSelectedItems`, `HostRunSessionSave`, `ClearMenu`, `ClearList`, `GetItemValue`, `GetItemText`, `SetDlgDefaultButton`, `SetDlgCancelButton`, `GetItemBounds`, `GetTextHeight`, `CenterDlg` — all return empty or zero. Any POSE code path that hits these (session save, preferences, debugging, error handling, Gremlins) will silently fail or produce garbage.

---

## Summary: Root Cause

The core problem is that the port was approached as **"replace FLTK widgets with Qt widgets"** rather than **"understand and replicate POSE's architecture in Qt."** The FLTK version has a clean `EmApplication` -> `EmApplicationFltk` subclass pattern with proper lifecycle management. The Qt version bypasses this entirely — it uses the base `EmApplication` directly, cherry-picks a few initialization steps into `PoseBridge`, scatters the rest across `main()` and `EmWindowQt`, and leaves the resulting gaps unfilled.

## Path Forward

1. Create `EmApplicationQt` that properly subclasses `EmApplication`, overrides `HandleIdle()` and provides a proper `Startup()`/`Shutdown()` lifecycle
2. Fix the initialization sequence to call `EmApplication::Startup()`
3. Address the 64-bit pointer truncation as a blocking issue, not a suppressed warning
4. Fix `omni_thread::self()` to return the correct thread identity
5. Add keyboard input handling
6. Port ReControl for automated testing support
7. Implement the dialog stubs or properly stub them with user-facing fallbacks
