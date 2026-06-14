# Phase 5 — Declutter and finalize 0.9.1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the verified-dead code and small duplications the Phase-0 audit
flagged, fix the normal-exit prefs-save bug, and finalize the 0.9.1 release
metadata + tag — WITHOUT building packages or running GATE 5 (both deferred to a
separate session per the 2026-06-14 scope decision).

**Architecture:** Linear, build-verified, one-commit-each. Every deletion is
grep-proven unreferenced in the compiled sources, then deleted, then the binary
is rebuilt and smoke-checked. Dedups change compiled code, so each gets a build
plus the relevant effect test. The "aggressive" dead-code sweep is a *bounded
discovery pass* (a hunter subagent produces a candidate list; only
high-confidence, build-clean candidates are applied, one commit each; uncertain
ones are logged, never silently deleted). A full regression sweep gates the
final 0.9.1 tag.

**Tech stack:** Qt6/C++17 emulator (legacy CMake build dir `build/`,
`make -C build -j15 pose64`), Python test harness (`tests/lib/harness.py`),
ReControl/MCP control plane.

---

## Verified ground truth (audited against live code 2026-06-14 — do not re-derive)

| Fact | Where |
|---|---|
| `EmApplication::Shutdown` calls `gPrefs->Save()` (writes `.poserrc`); `EmApplicationQt::Shutdown` does Qt prefs sync then calls it | `src/core/EmApplication.cpp:258-274`, `src/platform/EmApplicationQt.cpp:130-140` |
| `Startup` only `Load()`s prefs (does not create `.poserrc`); the prefs file is `.poserrc`, written next to the binary → `build/.poserrc` under the harness | `src/platform/EmApplicationQt.cpp:82-88`, `src/core/PreferenceMgr.cpp:1576` |
| `main.cpp` happy path returns `exitCode` at line 154 WITHOUT `theApp.Shutdown()`; the error path falls through to `theApp.Shutdown()` at line 166. `gCPUWorker->shutdown()` already ran (lines 143-149), so the CPU thread is down before any added Save() | `src/ui/main.cpp:140-170` |
| `quit` is `kCmdImmediate` → `RcCmd_Quit` → `gApplication->SetTimeToQuit(true)`; the idle timer then calls `QApplication::quit()`, `exec()` returns, normal-exit path runs | `src/core/ReControl.cpp:110`, `src/core/ReControlCmds_Session.cpp:103` |
| `JPEGToPixMap(EmStream&, EmPixMap&)` is a LIVE Qt/QImage reimplementation (no libjpeg); only caller is `EmWindow::GetSkin` (`EmWindow.cpp:880`) | `src/core/EmJPEG.cpp:14-48`, `src/core/EmWindow.cpp:871-888` |
| The `#ifndef DISABLE_JPEG_SUPPORT` blocks in `EmJPEG.h` guard the OLD libjpeg classes (`EmJPEGDecompress*`, `ConvertJPEG`) — `ConvertJPEG` has ZERO callers (grep); the bundled `src/core/jpeg/` (61 files) is excluded from the build and never compiled | `src/core/EmJPEG.h:17-165`, grep |
| `DISABLE_JPEG_SUPPORT` is referenced ONLY in `EmJPEG.h` (the `#ifndef`s) and `CMakeLists.txt:54` (the `-D` define). No `.cpp` reads it | grep |
| `src/core/Gzip/` (bundled) is excluded from the build (`CMakeLists.txt:139`) and NO file `#include`s any `src/core/Gzip/` header. `GzipEncode/Decode` live in `Miscellaneous.cpp` using SYSTEM zlib (`-lz`); only `SessionFile.cpp` uses them (via `Miscellaneous.h`) | `CMakeLists.txt:139`, grep |
| `ParseAddress` is DEFINED `src/core/ReControlCmds_Query.cpp:51` (external linkage) and `extern`-declared `src/core/ReControlCmds_Debug.cpp:25-26`; used at Query.cpp:422,479 and Debug.cpp:80,179,229. `ReControl.h` has no POSE types, but every consumer includes `EmCommon.h` (defines `emuptr`) before `ReControl.h` | grep, `src/core/ReControl.h` |
| The `#undef daysInYear` / `#undef monthsInYear` preamble appears in **12** files (recovery plan said 10): ReControl.cpp:25-26, ReControlCmds_Profile.cpp:7-8, ReControlCmds_Session.cpp:8-9, ReControlCmds_Input.cpp:7-8, ReControlCmds_Query.cpp:7-8, ReControlCmds_Debug.cpp:7-8, CPUWorkerThread.cpp:4-5, EmSession.cpp:45-46, EmApplicationQt.cpp:17-18, ui/main.cpp:26-27, EmWindowQt.cpp:21-22, EmDlgQt.cpp:35-36 (the latter also has unrelated `#undef ENTRY`/`#undef LENTRY` that stay put) | grep |
| `RcCmd_Profile` has 7 identical `EmSessionStopper stopper (gSession, kStopNow);` — one per subcommand (init:32, start:39, stop:46, dump:63, print:80, cleanup:87, cycles:94). `dump`/`print` run prerequisite-error returns BEFORE their stopper | `src/core/ReControlCmds_Profile.cpp` |
| Version is `0.9.1` in three places: `CMakeLists.txt:2` (`project(... VERSION 0.9.1)`), `main.cpp:53` (`setApplicationVersion`), `metainfo.xml:33` (`<release version="0.9.1" date="2026-03-13">`). The 0.9.1 release notes predate Phases 1–4.5 | files |
| UAE generator tools (`build68k.c`, `gencpu.c`) are NOT compiled (UAE_SOURCES = cpudefs.c/cpuemu.c/cpustbl.c/readcpu.cpp). **KEEP them** (user decision 2026-06-14: preserve regen ability) | `CMakeLists.txt:154-163` |

Project rules: LF-only files (`file <path>` to verify; `grep -lq $'\r'`);
reproduce/verify-first; one commit per deletion, build between (R-rule); honest
commit messages with trailer `Co-Authored-By: Claude Opus 4.8 (1M context)
<noreply@anthropic.com>`; docs in the same commit as the change they document
(R5); do not push until the final close-out.

**Standard quick smoke** (used after deletions — a deletion removes uncompiled
files, so build success is the primary gate; the smoke confirms nothing
linked-but-needed was removed):

```bash
make -C build -j15 pose64 2>&1 | tail -3   # must end "Built target pose64", no errors
# launch + basic liveness:
python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from tests.lib.harness import emulator, connect
with emulator(6457):
    c = connect(6457, timeout=10)
    try:
        assert (c.send_command("state") or "").startswith("OK"), "state not OK"
        h = c.send_command("screen-hash"); assert h and h.startswith("OK"), f"screen-hash: {h!r}"
        print("SMOKE OK", h.strip())
    finally:
        c.disconnect()
PY
```

**Full regression sweep** (gates the final tag — see Task 8 Step for the exact
command block).

---

### Task 1: Fix prefs-not-saved-on-normal-exit (`main.cpp`)

`main.cpp`'s happy path returns before `theApp.Shutdown()`, so `gPrefs->Save()`
never runs on a normal quit — preference changes during the session are lost.
The CPU worker is already shut down first, so adding the call is clear of the
landmine-#11a teardown race.

**Files:**
- Create: `tests/phase5/test_shutdown_saves_prefs.py`
- Modify: `src/ui/main.cpp:154` (add `theApp.Shutdown ();` before `return exitCode;`)

- [ ] **Step 1: Create the failing effect test**

Create `tests/phase5/test_shutdown_saves_prefs.py`:

```python
#!/usr/bin/env python3
"""Phase 5: a NORMAL quit must save preferences (EmApplication::Shutdown ->
gPrefs->Save()).  Pre-fix, main.cpp's happy path returns before
theApp.Shutdown(), so prefs are not written on normal exit.

Effect test (R3): run quietly for a moment so any startup-time Save() settles,
snapshot the .poserrc state, send `quit` (the normal-exit path:
SetTimeToQuit -> QApplication::quit -> exec() returns -> Shutdown), wait for the
process to exit on its own, then assert .poserrc was (re)written by Shutdown.
"""

import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6458
POSERRC = os.path.join(REPO, "build", ".poserrc")


def state(path):
    try:
        return os.stat(path).st_mtime_ns
    except FileNotFoundError:
        return None


def main():
    with emulator(PORT) as proc:
        c = connect(PORT, timeout=10)
        try:
            time.sleep(2.0)  # let any startup-time Save() settle
            before = state(POSERRC)
            # Normal-exit path. quit is kCmdImmediate; the reply may or may not
            # arrive before the socket drops, so don't assert on it.
            try:
                c.send_command("quit")
            except Exception:
                pass
        finally:
            c.disconnect()
        # Let the process exit on its own (NOT killed by the harness).
        proc.wait(timeout=15)

    after = state(POSERRC)
    assert after is not None, (
        f".poserrc absent after a normal quit ({POSERRC}) — Shutdown did not run")
    assert before != after, (
        ".poserrc was not rewritten across the normal quit "
        f"(mtime {before} unchanged) — Shutdown did not save prefs on exit")
    print(f"PASS (.poserrc rewritten by Shutdown on normal quit: {before} -> {after})")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it — must FAIL** (pre-fix Shutdown is skipped on normal exit)

```bash
timeout 120 python3 tests/phase5/test_shutdown_saves_prefs.py; echo "EXIT=$?"
```
Expected: `AssertionError` (`.poserrc absent…` or `…not rewritten…`), EXIT=1.

**If it does NOT fail** (i.e. `.poserrc` IS rewritten pre-fix), a startup-time
`Save()` fired between the snapshot and the quit. Fallback: change the test to
delete `build/.poserrc` BEFORE launch and assert it is ABSENT after the quit
pre-fix / PRESENT post-fix (Startup only `Load()`s, so only Shutdown creates
it). Record which variant discriminated.

- [ ] **Step 3: Apply the fix**

In `src/ui/main.cpp`, insert the Shutdown call in the happy path:

```cpp
			// Shut down ReControl server
			ReControl_Shutdown ();

			// Phase 5: save preferences on normal exit too.  The CPU worker
			// is already stopped (above), so this is clear of the teardown
			// race (STATUS landmine #11a).  The error path below also calls
			// Shutdown(); these are mutually exclusive (happy path returns
			// here), so it is never called twice.
			theApp.Shutdown ();

			return exitCode;
```

- [ ] **Step 4: Build and run the test — must PASS**

```bash
make -C build -j15 pose64 2>&1 | tail -3
timeout 120 python3 tests/phase5/test_shutdown_saves_prefs.py; echo "EXIT=$?"
```
Expected: `PASS (.poserrc rewritten by Shutdown on normal quit: …)`, EXIT=0.

- [ ] **Step 5: Quick regression (no double-Shutdown / clean exit code)**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" >/dev/null 2>&1 && echo "PASS $r" || echo "FAIL $r"; done
```
Expected: 7/7 PASS (these self-launch and tear down; a double-free at exit would
surface as a non-zero exit / crash).

- [ ] **Step 6: Commit**

```bash
git add src/ui/main.cpp tests/phase5/test_shutdown_saves_prefs.py
git commit -m "fix(phase5): save preferences on normal exit — main.cpp happy path skipped theApp.Shutdown()"
```

---

### Task 2: Delete the dead bundled JPEG library + collapse the DISABLE_JPEG_SUPPORT fiction

The bundled `src/core/jpeg/` (61 files) is never compiled; the libjpeg-based
classes behind `#ifndef DISABLE_JPEG_SUPPORT` (`EmJPEGDecompress*`,
`ConvertJPEG`) have zero callers. The real skin-image decode (`JPEGToPixMap`)
uses Qt's `QImage` and STAYS untouched.

**Files:**
- Delete: `src/core/jpeg/` (whole directory)
- Modify: `src/core/EmJPEG.h` (strip the dead `#ifndef DISABLE_JPEG_SUPPORT` blocks + the jmorecfg XMD_H/FAR cruft)
- Modify: `CMakeLists.txt` (remove the `-DDISABLE_JPEG_SUPPORT=1` define and the jpeg exclude-filter)

- [ ] **Step 1: Re-confirm nothing live needs the dead pieces**

```bash
grep -rn 'ConvertJPEG\|EmJPEGDecompress\|jpeglib\|jinclude\|jerror' src/ | grep -v 'src/core/jpeg/\|Emulator_Src_3.5'
grep -rn 'DISABLE_JPEG_SUPPORT' src/ CMakeLists.txt
```
Expected: the first prints nothing outside `EmJPEG.h`'s own dead blocks; the
second prints only `EmJPEG.h` (the `#ifndef`s) and `CMakeLists.txt:54`. If
`ConvertJPEG`/`EmJPEGDecompress*` appear with a real caller, STOP and reassess.

- [ ] **Step 2: Delete the bundled library**

```bash
git rm -r src/core/jpeg
```

- [ ] **Step 3: Simplify `EmJPEG.h`** to only the live declaration

Replace the entire body between the header guard so the file reads exactly:

```cpp
/* -*- mode: C++; tab-width: 4 -*- */
/* ===================================================================== *\
	Copyright (c) 1999-2001 Palm, Inc. or its subsidiaries.
	All rights reserved.

	This file is part of the Palm OS Emulator.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
\* ===================================================================== */

#ifndef EmJPEG_h
#define EmJPEG_h

// JPEG decoding is done by Qt's QImage (see EmJPEG.cpp / JPEGToPixMap).  The
// original bundled-libjpeg path (DISABLE_JPEG_SUPPORT, EmJPEGDecompress*,
// ConvertJPEG) was dead code and was removed in Phase 5.

class EmPixMap;
class EmStream;

// Utility function that converts a JPEG (or PNG/BMP — QImage auto-detects)
// from the stream to the given pixmap.

void	JPEGToPixMap	(EmStream&, EmPixMap&);

#endif	/* EmJPEG_h */
```

- [ ] **Step 4: Remove the CMake define and filter**

In `CMakeLists.txt`, delete these two lines from the `add_definitions(...)`
block (lines 53-54):

```cmake
    # Temporarily disable JPEG for initial port
    -DDISABLE_JPEG_SUPPORT=1
```

and delete the jpeg exclude-filter (lines 151-152):

```cmake
# Exclude bundled JPEG library — JPEG decoding uses Qt's QImage instead
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/jpeg/.*\\.c$")
```

- [ ] **Step 5: Reconfigure (sources changed), build, smoke**

A file was removed from the glob, so re-run cmake configure before building:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
make -C build -j15 pose64 2>&1 | tail -3
```
Then the **Standard quick smoke** (above). Expected: builds clean, `SMOKE OK`.

- [ ] **Step 6: Verify the skin/JPEG path still links and runs**

`JPEGToPixMap` is unchanged, but confirm a screenshot (which exercises the
window/pixmap path) still works:

```bash
python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from tests.lib.harness import emulator, connect
with emulator(6457):
    c = connect(6457, timeout=10)
    try:
        r = c.send_command("screenshot /tmp/pose5_jpeg_smoke.png")
        assert r and r.startswith("OK"), f"screenshot: {r!r}"
        import os; assert os.path.getsize("/tmp/pose5_jpeg_smoke.png") > 0
        print("SCREENSHOT OK")
    finally:
        c.disconnect()
PY
```
Expected: `SCREENSHOT OK`.

- [ ] **Step 7: Commit**

```bash
git add -A src/core/jpeg src/core/EmJPEG.h CMakeLists.txt
git commit -m "chore(phase5): delete dead bundled libjpeg + DISABLE_JPEG_SUPPORT fiction (Qt QImage does the decode)"
```

---

### Task 3: Delete the dead bundled Gzip directory

`src/core/Gzip/` is excluded from the build and no file `#include`s any header
from it. Session compression (`GzipEncode/Decode`) lives in `Miscellaneous.cpp`
on SYSTEM zlib and is untouched.

**Files:**
- Delete: `src/core/Gzip/` (whole directory)
- Modify: `CMakeLists.txt` (remove the now-moot Gzip exclude-filter)

- [ ] **Step 1: Re-confirm nothing includes a Gzip/ header**

```bash
grep -rn 'Gzip/' src/ --include=*.cpp --include=*.c --include=*.h | grep -v 'src/core/Gzip/\|Emulator_Src_3.5'
```
Expected: nothing. (Only `Miscellaneous.h`/`Miscellaneous.cpp` provide the
GzipEncode/Decode declarations/definitions, and those files are NOT under
`src/core/Gzip/`.)

- [ ] **Step 2: Delete and drop the filter**

```bash
git rm -r src/core/Gzip
```
Then in `CMakeLists.txt` delete the now-moot filter (lines 138-139):

```cmake
# Exclude bundled Gzip — symbols collide with system zlib
list(FILTER CORE_SOURCES EXCLUDE REGEX "src/core/Gzip/.*\\.(c|cpp)$")
```

- [ ] **Step 3: Reconfigure, build, smoke**

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
make -C build -j15 pose64 2>&1 | tail -3
```
Then the **Standard quick smoke**. Expected: builds clean, `SMOKE OK`.

- [ ] **Step 4: Verify session compression still works (save/load round-trip)**

`GzipEncode/Decode` are used by session save/restore:

```bash
python3 - <<'PY'
import sys, os; sys.path.insert(0, ".")
from tests.lib.harness import emulator, connect
P="/tmp/pose5_gzip_smoke.psf"
if os.path.exists(P): os.remove(P)
with emulator(6457):
    c = connect(6457, timeout=10)
    try:
        r = c.send_command(f"save {P}"); assert r and r.startswith("OK"), f"save: {r!r}"
        assert os.path.getsize(P) > 0, "empty session file"
        r = c.send_command(f"load {P}"); assert r and r.startswith("OK"), f"load: {r!r}"
        assert (c.send_command("state") or "").startswith("OK")
        print("SESSION ROUNDTRIP OK", os.path.getsize(P))
    finally:
        c.disconnect()
PY
```
Expected: `SESSION ROUNDTRIP OK <bytes>`. (If `save`/`load` arg syntax differs,
check `docs/recontrol-protocol.md` and adjust — the point is a save then load
both return OK.)

- [ ] **Step 5: Commit**

```bash
git add -A src/core/Gzip CMakeLists.txt
git commit -m "chore(phase5): delete dead bundled Gzip dir (session compression uses system zlib)"
```

---

### Task 4: Aggressive dead-code sweep (bounded discovery → verified apply)

Find additional removable dead code beyond the known items, but only delete
high-confidence, build-clean candidates — one commit each. Uncertain candidates
are LOGGED, not deleted (no silent over-deletion; the project's history warns
against unverified bulk changes).

**Files:** discovered at runtime; each candidate is its own small commit.

- [ ] **Step 1: Dispatch a dead-code hunter subagent**

Use the Agent tool (subagent_type `Explore`) with this prompt:

> Read-only dead-code hunt in /home/clinton/dev/POSE64 (Qt6/C++ POSE port).
> Find removable dead code and report a RANKED candidate list with evidence —
> do NOT change anything. For each candidate give: path, what it is, and the
> exact grep/CMake evidence that it is unreferenced by the COMPILED build.
> Look for: (a) `.c`/`.cpp` files present on disk but NOT in any CMake source
> list or excluded by a `list(FILTER ... EXCLUDE ...)` (compare the
> `file(GLOB ...)` results + explicit `set(... _SOURCES ...)` against
> CMakeLists.txt — note POSE compiles via globs at src/core/*.cpp and
> src/core/*.c, plus explicit UAE_SOURCES and PLATFORM_SOURCES); (b) headers
> never `#include`d anywhere; (c) functions/classes with a single declaration
> and zero references; (d) whole helper modules superseded by Qt (like the
> jpeg/Gzip cases). EXCLUDE from candidates: anything under
> `src/Emulator_Src_3.5/`, `abandoned/`, `pose32bit/`, `src/fltk*` (reference
> trees, already gitignored); the UAE generator tools `build68k.c`/`gencpu.c`
> (deliberately KEPT for regen); generated UAE output (cpuemu.c/cpustbl.c/
> cputbl.h); and anything reachable from a compiled TU. Rank each
> HIGH/MEDIUM/LOW confidence and explain the residual doubt for MEDIUM/LOW.

- [ ] **Step 2: Triage the candidate list**

For each HIGH-confidence candidate, independently re-verify before trusting it:

```bash
# Example verification for a candidate file src/core/Foo.cpp:
grep -rn 'Foo\b' src/ CMakeLists.txt | grep -v 'src/core/Foo.cpp\|Emulator_Src_3.5'
grep -n 'Foo' build/compile_commands.json   # must NOT appear as a compiled TU
```
Keep only candidates with zero compiled references. MEDIUM/LOW → log them in
Step 5, do not delete.

- [ ] **Step 3: Apply each verified candidate — one commit each, build between**

For each kept candidate:

```bash
git rm <candidate>                 # or remove the dead function/decl via Edit
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1   # if a source file was removed
make -C build -j15 pose64 2>&1 | tail -3      # MUST stay clean
```
If the build breaks, `git checkout -- .` (or `git restore --staged --worktree`)
that candidate and move it to the logged list — it was not actually dead.
On a clean build run the **Standard quick smoke**, then:

```bash
git add -A
git commit -m "chore(phase5): remove dead <thing> (unreferenced; build-verified)"
```

- [ ] **Step 4: If the sweep finds nothing high-confidence**

That is a valid outcome — record it (`log()`/note) and move on. The known
deletions (Tasks 2-3) are the bulk; do not manufacture deletions.

- [ ] **Step 5: Log what was deliberately NOT deleted**

Add a short bullet list (candidate + why deferred) to the Phase 5 STATUS update
in Task 8 so the next pass has the trail. No silent truncation.

---

### Task 5: Dedup `ParseAddress` into `ReControl.h`

Replace the duplicated `extern` declaration in `ReControlCmds_Debug.cpp` with a
single shared declaration in `ReControl.h`.

**Files:**
- Modify: `src/core/ReControl.h` (add the declaration)
- Modify: `src/core/ReControlCmds_Debug.cpp:25-26` (remove the `extern`)

- [ ] **Step 1: Add the shared declaration to `ReControl.h`**

Before the final `#endif /* ReControl_h */`, after the Public API section, add:

```cpp
// ---------------------------------------------------------------------------
// Shared command helpers
// ---------------------------------------------------------------------------
// Parses a hex/decimal address string into emuptr.  Defined in
// ReControlCmds_Query.cpp.  (emuptr comes from EmCommon.h, which every
// ReControl consumer includes before ReControl.h.)
bool ParseAddress (const std::string& addrStr, emuptr& outAddr);
```

- [ ] **Step 2: Remove the duplicate `extern` from `ReControlCmds_Debug.cpp`**

Delete lines 25-26:

```cpp
// ParseAddress is defined in ReControlCmds_Query.cpp
extern bool ParseAddress (const std::string& addrStr, emuptr& outAddr);
```
(`ReControlCmds_Debug.cpp` already includes `ReControl.h`, so the declaration
now comes from there.)

- [ ] **Step 3: Build — must stay clean**

```bash
make -C build -j15 pose64 2>&1 | tail -3
```
Expected: clean build. If `emuptr` is undefined in some TU that includes
`ReControl.h` without `EmCommon.h` first, the build will say so — in that case
add `#include "EmCommon.h"` is wrong for a header; instead include the minimal
type header `#include "EmTypes.h"` at the top of `ReControl.h` and rebuild.
(Confirm against the build which path was needed.)

- [ ] **Step 4: Verify the address-parsing commands still work (effect)**

`peek` and `poke` go through `ParseAddress`:

```bash
python3 - <<'PY'
import sys; sys.path.insert(0, ".")
from tests.lib.harness import emulator, connect
with emulator(6457):
    c = connect(6457, timeout=10)
    try:
        r = c.send_command("peek 0x10c00000 4")   # ROM base region; any valid addr
        assert r and r.startswith("OK"), f"peek good addr: {r!r}"
        r = c.send_command("peek notanaddr 4")
        assert r and r.startswith("ERR"), f"peek bad addr should ERR: {r!r}"
        print("PARSEADDR OK", r.strip())
    finally:
        c.disconnect()
PY
```
Expected: a good address peeks OK, a bad one returns ERR (ParseAddress rejects
it) → `PARSEADDR OK`.

- [ ] **Step 5: Commit**

```bash
git add src/core/ReControl.h src/core/ReControlCmds_Debug.cpp
git commit -m "refactor(phase5): declare ParseAddress once in ReControl.h (drop duplicate extern)"
```

---

### Task 6: Hoist the `#undef daysInYear`/`monthsInYear` preamble into a shim header

12 files repeat the same two `#undef`s to defuse a Palm-vs-Qt macro clash.
Centralize them in one shim header.

**Files:**
- Create: `src/core/PalmMacroUndefs.h`
- Modify (replace the 2-3 line preamble with one `#include`): ReControl.cpp,
  ReControlCmds_Profile.cpp, ReControlCmds_Session.cpp, ReControlCmds_Input.cpp,
  ReControlCmds_Query.cpp, ReControlCmds_Debug.cpp, CPUWorkerThread.cpp,
  EmSession.cpp (all in `src/core/`); EmApplicationQt.cpp, EmWindowQt.cpp,
  EmDlgQt.cpp (in `src/platform/`); main.cpp (in `src/ui/`)

- [ ] **Step 1: Create the shim header**

Create `src/core/PalmMacroUndefs.h` (no include guard on purpose — re-running
the `#undef`s is a harmless no-op, and a guard could let a later re-include of
the Palm headers leave the macros defined):

```cpp
/* -*- mode: C++; tab-width: 4 -*- */
/* Phase 5: single home for the Palm-OS-vs-Qt macro clash defusal.  Palm's
 * DateTime.h does `#define daysInYear 365` (and monthsInYear), which collides
 * with Qt usage.  Include this AFTER the Palm headers (i.e. after EmCommon.h)
 * and BEFORE the Qt headers, exactly where the old per-file preamble sat.
 * Intentionally has no include guard. */

#undef daysInYear
#undef monthsInYear
```

- [ ] **Step 2: Replace the preamble in each of the 12 files**

In each file listed above, replace its preamble block — the optional comment
line plus the two `#undef` lines, e.g.:

```cpp
// Undefine Palm OS macros that conflict with Qt
#undef daysInYear
#undef monthsInYear
```

with a single include (keep it in the SAME position — after `EmCommon.h`,
before any Qt header):

```cpp
#include "PalmMacroUndefs.h"	// Phase 5: daysInYear/monthsInYear undef
```

Do NOT touch `EmDlgQt.cpp`'s unrelated `#undef ENTRY` / `#undef LENTRY`
(lines ~856 / ~1335) — only its `daysInYear`/`monthsInYear` preamble.

- [ ] **Step 3: Confirm the macros are gone from line-noise and build**

```bash
grep -rn 'undef daysInYear\|undef monthsInYear' src/ | grep -v PalmMacroUndefs.h
make -C build -j15 pose64 2>&1 | tail -3
```
Expected: the grep prints nothing (all 12 preambles replaced); build is clean.
A leftover Palm-macro clash would surface as a compile error in the affected TU.

- [ ] **Step 4: Quick smoke**

Run the **Standard quick smoke**. Expected: `SMOKE OK`.

- [ ] **Step 5: Verify LF + commit**

```bash
git add src/core/PalmMacroUndefs.h src/core/ReControl.cpp \
  src/core/ReControlCmds_Profile.cpp src/core/ReControlCmds_Session.cpp \
  src/core/ReControlCmds_Input.cpp src/core/ReControlCmds_Query.cpp \
  src/core/ReControlCmds_Debug.cpp src/core/CPUWorkerThread.cpp \
  src/core/EmSession.cpp src/platform/EmApplicationQt.cpp \
  src/platform/EmWindowQt.cpp src/platform/EmDlgQt.cpp src/ui/main.cpp
git commit -m "refactor(phase5): hoist daysInYear/monthsInYear undef into one shim header (12 files)"
```

---

### Task 7: Hoist the 7 duplicate profile stoppers into one

`RcCmd_Profile` creates an identical `EmSessionStopper` in each of its 7
subcommands. Hoist to a single stopper. The only behavioral delta: `dump`/`print`
prerequisite-error returns now occur with a momentary session stop — the same
`kStopNow` every profile subcommand already uses, destroyed immediately on the
error return, so it resumes at once. Correctness is unchanged.

**Files:**
- Modify: `src/core/ReControlCmds_Profile.cpp`

- [ ] **Step 1: Restructure `RcCmd_Profile`**

Replace the function body (lines 16-103, keeping the `#if HAS_PROFILING` wrapper)
so there is exactly one stopper, created after the usage check:

```cpp
std::string RcCmd_Profile (const QStringList& args)
{
	if (args.size () < 2)
		return "ERR usage: profile <init|start|stop|dump|print|cleanup|cycles>\n";

	QString sub = args[1].toLower ();

	// dump/print validate prerequisites and may return an error before doing
	// any work; those returns happen before the stopper does anything costly.
	std::string dumpPath;
	if (sub == "dump" || sub == "print")
	{
		if (args.size () < 3)
			return "ERR usage: profile " + sub.toStdString () + " <path>\n";
		dumpPath = args[2].toStdString ();
		if (!gProfilingEnabled)
			return "ERR transient: profiling not enabled (call profile init + start first)\n";
		if (gProfilingOn)
			return "ERR transient: profiling still running (call profile stop first)\n";
		if (gClockCycles == 0)
			return "ERR transient: no profiling data collected (run CPU with profiling enabled first)\n";
	}

	// One stopper for all subcommand work (every branch needs kStopNow).
	EmSessionStopper stopper (gSession, kStopNow);

	if (sub == "init")
	{
		int maxCalls = MAXFNCALLS;
		int maxDepth = 200;
		if (args.size () >= 3) maxCalls = args[2].toInt ();
		if (args.size () >= 4) maxDepth = args[3].toInt ();
		if (maxCalls < 1) maxCalls = MAXFNCALLS;
		if (maxDepth < 1) maxDepth = 200;
		ProfileInit (maxCalls, maxDepth);
		return "OK\n";
	}

	if (sub == "start")
	{
		ProfileStart ();
		return "OK\n";
	}

	if (sub == "stop")
	{
		ProfileStop ();
		return "OK\n";
	}

	if (sub == "dump")
	{
		ProfileDump (dumpPath.c_str ());
		return "OK\n";
	}

	if (sub == "print")
	{
		ProfilePrint (dumpPath.c_str ());
		return "OK\n";
	}

	if (sub == "cleanup")
	{
		ProfileCleanup ();
		return "OK\n";
	}

	if (sub == "cycles")
	{
		char buf[128];
		snprintf (buf, sizeof (buf), "OK clock=%lld read=%lld write=%lld\n",
			(long long) gClockCycles, (long long) gReadCycles, (long long) gWriteCycles);
		return std::string (buf);
	}

	return "ERR usage: profile <init|start|stop|dump|print|cleanup|cycles>\n";
}
```

- [ ] **Step 2: Build**

```bash
make -C build -j15 pose64 2>&1 | tail -3
```
Expected: clean build.

- [ ] **Step 3: Verify the full profile lifecycle still works (effect)**

```bash
python3 - <<'PY'
import sys, os, time; sys.path.insert(0, ".")
from tests.lib.harness import emulator, connect
OUT="/tmp/pose5_profile.mwp"
if os.path.exists(OUT): os.remove(OUT)
with emulator(6457):
    c = connect(6457, timeout=10)
    try:
        # error path before init still returns ERR (and no longer hangs)
        r = c.send_command("profile dump "+OUT); assert r.startswith("ERR"), f"pre-init dump: {r!r}"
        assert c.send_command("profile init").startswith("OK")
        assert c.send_command("profile start").startswith("OK")
        c.send_command("run"); time.sleep(2.0); c.send_command("break")  # collect some cycles
        assert c.send_command("profile stop").startswith("OK")
        cyc = c.send_command("profile cycles"); assert cyc.startswith("OK clock="), f"cycles: {cyc!r}"
        d = c.send_command("profile dump "+OUT); assert d.startswith("OK"), f"dump: {d!r}"
        assert os.path.getsize(OUT) > 0, "empty .mwp"
        assert c.send_command("profile cleanup").startswith("OK")
        print("PROFILE LIFECYCLE OK", cyc.strip())
    finally:
        c.disconnect()
PY
```
Expected: `PROFILE LIFECYCLE OK clock=…`. (If `run`/`break` arg forms differ,
consult `docs/recontrol-protocol.md`; the point is init→start→collect→stop→
cycles→dump→cleanup all OK and the pre-init dump still ERRs.)

- [ ] **Step 4: Commit**

```bash
git add src/core/ReControlCmds_Profile.cpp
git commit -m "refactor(phase5): hoist RcCmd_Profile's 7 duplicate session stoppers into one"
```

---

### Task 8: Finalize 0.9.1 — release notes, STATUS, banner, sweep, tag, push

No version bump (0.9.1 stays); refresh the 0.9.1 release entry to reflect what
0.9.1 actually contains (Phases 1–4.5), update the docs, run the full sweep,
tag, and push. Packaging and GATE 5 are explicitly deferred.

**Files:**
- Modify: `data/ca.vibekoder.pose64.metainfo.xml` (refresh the 0.9.1 release block)
- Modify: `docs/STATUS.md` (Phase 5 progress + header; record any logged
  non-deletions from Task 4)
- Modify: `docs/recovery-plan-2026-06.md` (banner: Phase 5 cleanup done; GATE 5
  + packaging pending)

- [ ] **Step 1: Refresh the 0.9.1 release notes + date**

In `data/ca.vibekoder.pose64.metainfo.xml`, replace the 0.9.1 `<release>` block
(lines 33-39) with the real 0.9.1 content and today's date:

```xml
    <release version="0.9.1" date="2026-06-14">
      <description>
        <p>Stabilization release. Killed the cross-thread freeze classes
        (suspend-counter leak, untimed stops, nested-ROM-call abortion,
        dialog-action lifetime); made input delivery honest (delivered-ACK with
        one wake mechanism); rebuilt the MCP debug surface (backtrace,
        breakpoints, watchpoints, logging, gremlins, memory checks, error
        handling, profiling) as 37 source-of-truth tools; root-fixed the
        MetaMemory check freeze; and demonstrated HotSync against pilot-link
        over a virtual serial PTY (launch, attach, tap once).</p>
      </description>
    </release>
```

(Leave the 0.9.0 block unchanged. CMakeLists.txt:2 and main.cpp:53 already say
0.9.1 — no change needed there.)

- [ ] **Step 2: Update `docs/STATUS.md`**

- Header date line: append "Phase 5 (declutter) cleanup COMPLETE 2026-06-14;
  GATE 5 + packaging pending."
- Add a Phase 5 bullet to "Recovery progress" listing: the Shutdown fix, the
  JPEG + Gzip deletions (+ any Task-4 sweep deletions), the three dedups, and
  the logged-but-NOT-deleted candidates from Task 4 Step 5.
- Note that 0.9.1 is finalized (notes refreshed, tagged) but packages are not
  yet built and GATE 5 has not been run — both are the next session.

- [ ] **Step 3: Update the recovery-plan banner**

In `docs/recovery-plan-2026-06.md`:
- Bump the "updated" date to 2026-06-14.
- Add a Phase 5 block: cleanup + 0.9.1 finalize COMPLETE (list the deletions,
  dedups, Shutdown fix, tag 0.9.1); **NEXT ACTION = Phase 5 remainder: build
  packages (deb/AppImage/exe) + run GATE 5** (the v1.0 definition of done).
- In the Phase 5 task list near the bottom, check off the deletion/dedup/
  Shutdown items; leave the packaging + GATE 5 items unchecked.

- [ ] **Step 4: Full regression sweep (gates the tag)**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" >/dev/null 2>&1 && echo "PASS $r" || echo "FAIL $r"; done
python3 tests/phase2/test_honest_ack.py >/dev/null 2>&1 && echo "PASS honest_ack" || echo "FAIL honest_ack"
timeout 120 python3 tests/phase3/test_mcp_surface.py 2>&1 | tail -1
timeout 300 python3 tests/phase3/test_mcp_dispatch.py 2>&1 | tail -1
timeout 300 python3 tests/phase3/test_break_blocked_ops.py 2>&1 | tail -1
timeout 120 python3 tests/phase4/test_info_serial.py 2>&1 | tail -1
timeout 120 python3 tests/phase4/test_preference_cli.py 2>&1 | tail -1
timeout 120 python3 tests/phase4/test_pty_stale_flush.py 2>&1 | tail -1
timeout 300 python3 tests/phase4/test_hotsync_smoke.py 2>&1 | tail -1
timeout 600 python3 tests/phase4/test_hotsync_soak.py 2>&1 | tail -1
timeout 120 python3 tests/phase5/test_shutdown_saves_prefs.py 2>&1 | tail -1
```
Expected: everything PASS. Anything red blocks the tag.

- [ ] **Step 5: Verify LF on all touched docs, commit, tag, push**

```bash
for f in data/ca.vibekoder.pose64.metainfo.xml docs/STATUS.md docs/recovery-plan-2026-06.md; do
  grep -lq $'\r' "$f" && echo "CRLF: $f" || echo "LF ok: $f"
done
git add data/ca.vibekoder.pose64.metainfo.xml docs/STATUS.md docs/recovery-plan-2026-06.md
git commit -m "docs(phase5): finalize 0.9.1 release notes + STATUS/banner (cleanup done; packaging + GATE 5 next)"
git tag 0.9.1
git push && git push --tags
```
Expected: clean push; `0.9.1` tag on origin. (First confirm no existing `0.9.1`
tag: `git tag --list 0.9.1` should be empty before tagging.)

---

## Explicitly out of scope (deferred to the next session)

- **Building packages** — deb (`cpack -G DEB`), AppImage
  (`scripts/build-appimage.sh`), Windows exe. The machinery exists; running it
  is environment-heavy and belongs with GATE 5.
- **GATE 5** (= v1.0 definition of done) — fresh-clone build, packaging, and a
  30-minute autonomous agent session (install/launch/crash/inspect/recover ×20)
  with zero restarts. This is the gate the whole recovery plan builds toward;
  it deserves its own focused run.
- **UAE generator tools** (`build68k.c`, `gencpu.c`) — KEPT to preserve the
  ability to regenerate `cpuemu.c`/`cpustbl.c` (user decision 2026-06-14).
- **Any version bump past 0.9.1** — this session finalizes 0.9.1 as-is.
```
