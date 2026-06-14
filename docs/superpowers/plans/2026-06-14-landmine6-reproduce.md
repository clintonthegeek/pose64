# Landmine #6 — Reproduce the PaintScreen / gMemAccessFlags TSAN Race

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a committed, repeatable test that demonstrates landmine #6 — the data race between the main-thread `EmWindow::PaintScreen` LCD read and the CPU thread, via `CEnableFullAccess`'s non-atomic writes to the global `gMemAccessFlags` — using ThreadSanitizer. This satisfies recovery-plan rule R1 (a failing reproduction lands before any fix).

**Architecture:** The phase tests self-launch `build/pose64` headless. This race only fires on the real GUI paint path, so the repro launches the existing ThreadSanitizer build (`build-tsan/`, already configured `-fsanitize=thread -O1 -g`) under a **real** X display (DISPLAY :1), drives guest CPU + screen churn with a gremlin, captures TSAN's stderr via the harness `capture_log`, and asserts a data-race report rooted in the paint path is present. Convention (phase-1 repro): exit 0 = signature absent (fixed), exit 1 = signature present (reproduced). Pre-fix this MUST exit 1.

**Tech Stack:** C++/Qt6 emulator; Python 3 test harness (`tests/lib/harness.py`, no pytest); Clang ThreadSanitizer; CMake.

**Scope:** Phase 1 (reproduce) ONLY. The fix is a separate plan (`2026-06-14-landmine6-fix.md`) authored after this repro runs, because the spec's CRITICAL FINDING — `PaintScreen` deliberately omits the CPU stop to avoid a documented deadlock (`EmWindow.cpp:485-489`) — means the fix approach (thread-safe `gMemAccessFlags` vs a bounded deadlock-safe stop) must be chosen and measured against this repro, not guessed now. See `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md`.

---

## File Structure

- **Modify** `tests/lib/harness.py` — add a backward-compatible `ready_timeout` parameter to `emulator()` (slow sanitizer builds can exceed the 25 s boot-wait default). Reused by the #5 repro later.
- **Create** `tests/phase6/__init__.py` — empty package marker (new phase dir for post-1.0 hardening repros).
- **Create** `tests/phase6/repro_landmine6_paint_race.py` — the repro.
- **Artifact (not committed)** `build-tsan/` — refreshed against the current branch.
- **Modify** `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md` — paste the captured TSAN signature as evidence.

---

### Task 1: Make the harness boot-wait timeout configurable (slow TSAN builds)

**Files:**
- Modify: `tests/lib/harness.py` (the `emulator()` context manager, ~line 40)

- [ ] **Step 1: Add the `ready_timeout` parameter and pass it through**

Change the `emulator()` signature and its `wait_ready` call. Current:

```python
@contextlib.contextmanager
def emulator(port, psf="m515.psf", build="build", env_extra=None, capture_log=None,
             extra_args=None):
```

to:

```python
@contextlib.contextmanager
def emulator(port, psf="m515.psf", build="build", env_extra=None, capture_log=None,
             extra_args=None, ready_timeout=25):
```

and change the readiness check inside the `try:` from:

```python
        if not wait_ready(port):
            raise RuntimeError(f"server not ready on port {port}")
```

to:

```python
        if not wait_ready(port, timeout=ready_timeout):
            raise RuntimeError(f"server not ready on port {port}")
```

(`wait_ready` already accepts `timeout=25`; no change needed there.)

- [ ] **Step 2: Confirm no regression on a fast existing repro**

Run: `python3 tests/phase1/repro_1_8_argval.py`
Expected: prints a line starting `PASS 1.8`, exit 0.

- [ ] **Step 3: Commit**

```bash
git add tests/lib/harness.py
git commit -m "test(harness): configurable ready_timeout for slow sanitizer builds

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Refresh `build-tsan` against the current branch

**Files:** `build-tsan/` (build artifact; not tracked by git)

- [ ] **Step 1: Rebuild the ThreadSanitizer binary**

Run: `cmake --build build-tsan --parallel "$(nproc)" 2>&1 | tail -8`
Expected: ends with a successful build (e.g. `[100%] Built target pose64`), no compile errors. (If unchanged since last build it relinks quickly.)

If `build-tsan/` is missing or its cache is wrong, recreate it first:
```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build-tsan --parallel "$(nproc)"
```

- [ ] **Step 2: Sanity-check the binary exists and is a TSAN build**

Run: `test -x build-tsan/pose64 && grep -q "fsanitize=thread" build-tsan/CMakeCache.txt && echo OK`
Expected: `OK`

(No commit — `build-tsan/` is a build artifact.)

---

### Task 3: Create the phase6 package and the repro test

**Files:**
- Create: `tests/phase6/__init__.py`
- Create: `tests/phase6/repro_landmine6_paint_race.py`

- [ ] **Step 1: Create the empty package marker**

Create `tests/phase6/__init__.py` with no content (empty file).

- [ ] **Step 2: Write the repro**

Create `tests/phase6/repro_landmine6_paint_race.py`:

```python
#!/usr/bin/env python3
"""Landmine #6 reproduction (recovery-plan R1).

EmWindow::PaintScreen (main thread, EmWindow.cpp:545) reads the LCD via
EmScreen::GetBits without stopping the CPU.  GetBits constructs a temporary
CEnableFullAccess, whose ctor/dtor do non-atomic writes to the PROCESS-GLOBAL
gMemAccessFlags (EmMemory.cpp:635 / :651) that the CPU thread reads on every
guest memory access.  Data race.

The stop was deliberately removed to avoid a SuspendThread deadlock against a
nested CPU (EmWindow.cpp:485-489), so this is reproduced (not "fixed") under
ThreadSanitizer rather than by adding a stopper.

Method: launch the build-tsan binary under a REAL X display (offscreen does
not paint), churn CPU + screen with a gremlin, capture TSAN's stderr, and look
for a data-race report whose stack names the paint path AND the access-flag
swap.  Match is SPECIFIC (not "any race") because other baseline race families
(landmine #5) persist until later.

Convention: exit 1 = signature PRESENT (bug reproduced, RED);
            exit 0 = signature ABSENT (bug fixed, GREEN).
Running this pre-fix MUST exit 1.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6437
TSAN_LOG = "/tmp/pose64-landmine6-tsan.log"
DURATION_S = 30          # TSAN is slow; give the race time to fire under load
DISPLAY = os.environ.get("DISPLAY", ":1")


def find_signature(text):
    """Return the first TSAN data-race block rooted in the paint path, or None."""
    blocks = re.split(r"(?=WARNING: ThreadSanitizer: data race)", text)
    for b in blocks:
        if "ThreadSanitizer: data race" not in b:
            continue
        if "PaintScreen" in b and ("gMemAccessFlags" in b or "CEnableFullAccess" in b):
            return b
    return None


def main():
    env_extra = {
        "QT_QPA_PLATFORM": "xcb",          # override the harness offscreen default
        "DISPLAY": DISPLAY,
        # keep running after each report, deep stacks, don't fail the process
        "TSAN_OPTIONS": "halt_on_error=0 history_size=7 exitcode=0",
    }
    with emulator(PORT, build="build-tsan", env_extra=env_extra,
                  capture_log=TSAN_LOG, ready_timeout=120) as proc:
        c = connect(PORT, timeout=30)
        try:
            r = c.send_command("gremlin new 42 1000000")
            assert r.startswith("OK"), f"gremlin failed to start: {r!r}"
            time.sleep(DURATION_S)
            c.send_command("gremlin stop")
        finally:
            try:
                c.send_command("quit")
            except Exception:
                pass
            c.disconnect()
        try:
            proc.wait(timeout=20)   # let TSAN flush reports as it exits
        except Exception:
            pass

    with open(TSAN_LOG, "r", errors="replace") as fh:
        text = fh.read()

    total = text.count("WARNING: ThreadSanitizer: data race")
    block = find_signature(text)
    print(f"TSAN data-race reports total: {total}; log: {TSAN_LOG}")
    if block:
        print("REPRO CONFIRMED — landmine #6 paint-path race present:\n")
        print("\n".join(block.splitlines()[:25]))
        sys.exit(1)
    if total == 0:
        print("NO races at all — the run likely did not PAINT.")
        print("Check: DISPLAY is a real X server, QT_QPA_PLATFORM=xcb, window mapped.")
    else:
        print("Races present but none on the paint path this run "
              "(try a longer DURATION_S).")
    sys.exit(0)


if __name__ == "__main__":
    main()
```

(No run in this task.)

---

### Task 4: Run the repro and confirm it reproduces (RED)

- [ ] **Step 1: Run it**

Run: `python3 tests/phase6/repro_landmine6_paint_race.py`
Expected: a line `TSAN data-race reports total: N` (N > 0), then
`REPRO CONFIRMED — landmine #6 paint-path race present:` and a TSAN block whose
frames include `EmWindow::PaintScreen` and `CEnableFullAccess`/`gMemAccessFlags`.
Process exit code **1** (this is the reproduction).

- [ ] **Step 2: If it does NOT reproduce, widen the window and verify painting**

If output says "NO races at all — the run likely did not PAINT":
- Confirm DISPLAY :1 is a live X server: `DISPLAY=:1 xdpyinfo >/dev/null && echo display-ok`
- Confirm a window maps: launch is GUI (xcb); if headless-only, start `Xvfb :1 -screen 0 1024x768x24 &` first.

If "Races present but none on the paint path": edit `DURATION_S = 60` in the test and re-run Step 1. Re-run until the paint-path signature appears (the race is probabilistic but frequent under gremlin churn).

(No commit until the signature is captured.)

---

### Task 5: Record evidence and commit the failing repro (R1 gate)

**Files:**
- Modify: `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md`

- [ ] **Step 1: Capture the signature into the spec as evidence**

Copy the first paint-path race block from `/tmp/pose64-landmine6-tsan.log` and paste it into the spec under the `### Reproduce` section as a fenced `Evidence (captured <date>)` block (the exact TSAN frames, so the fix plan can assert the same signature disappears). Keep it to ~20 lines.

- [ ] **Step 2: Verify Unix line endings on any file written**

Run: `file tests/phase6/repro_landmine6_paint_race.py docs/superpowers/specs/2026-06-14-landmine-hardening-design.md`
Expected: no "CRLF". If any shows CRLF: `sed -i 's/\r$//' <path>`.

- [ ] **Step 3: Commit the failing repro**

```bash
git add tests/phase6/ docs/superpowers/specs/2026-06-14-landmine-hardening-design.md
git commit -m "test(landmine6): reproduce PaintScreen/gMemAccessFlags TSAN race (RED, R1)

build-tsan under a real display + gremlin churn surfaces a data race whose
stack names EmWindow::PaintScreen and CEnableFullAccess's write to the global
gMemAccessFlags. Exit 1 = race present; flips to exit 0 when the fix lands.
Fix approach deferred to the follow-on plan (spec CRITICAL FINDING: the stop
was deliberately removed to avoid a nested-CPU deadlock).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## After this plan

The fix is the next plan (`2026-06-14-landmine6-fix.md`), authored once the repro is green-able. Its first task is the gMemAccessFlags reader/writer survey (already enumerated: `EmBankSRAM/Regs/ROM/DRAM/Dummy.cpp`, `EmRegsEZ/328/VZ.cpp`, `EmMemory.{h,cpp}`) to validate the thread-local approach (spec approach 1) before the bounded-stop fallback (approach 2).

---

## Self-Review

**Spec coverage:** This plan implements the spec's `### Reproduce` section in full (build-tsan + real display + gremlin + specific-signature capture + R1 commit) and the harness-gap note (Task 1). The spec's fix approaches are intentionally out of scope (deferred to the fix plan) — stated in Scope.

**Placeholder scan:** No "TBD"/"handle edge cases"/"similar to". The test code is complete; commands are exact with expected output. The one judgement call (which fix approach) is explicitly deferred, not hand-waved.

**Type/name consistency:** `emulator(..., ready_timeout=...)` defined in Task 1 and used in Task 3; `find_signature`/`TSAN_LOG`/`DURATION_S`/`PORT` consistent within the test; import path `tests.lib.harness` matches the canonical module verified to expose `emulator`/`connect`.
