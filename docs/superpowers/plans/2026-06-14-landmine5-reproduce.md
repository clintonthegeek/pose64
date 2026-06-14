# Landmine #5 — Reproduce the Two-Thread 68K Core Race

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a committed, repeatable test that demonstrates landmine #5 — a second `SuspendThread(kStopOnSysCall)` caller claiming a stopped-on-syscall CPU it did not stop (`EmSession.cpp:998-1004`, no ownership guard; the window opens because `ExecuteSubroutine` releases `fSharedLock` during `CallCPU()` at `:1343-1346`), letting two code paths drive UAE's global `regs` concurrently. Satisfies R1 before any fix.

**Architecture:** This race needs **two concurrent CPU-driving paths** — a worker-thread ReControl ROM call (`ExecuteSubroutine` via e.g. `apps`/`launch`) overlapping a main-thread ROM/idle path. ReControl is single-connection, so the second driver is a main-thread path; identifying it is the first real task. Reproduce under the ThreadSanitizer build (`build-tsan/`) on a real X display (DISPLAY :1) and assert TSAN reports a data race on the m68k register globals with execution-path frames from two different threads.

**Tech Stack:** C++/Qt6 emulator; Python 3 harness (`tests/lib/harness.py`); Clang ThreadSanitizer; CMake.

**Scope:** Phase 1 (reproduce) ONLY. The fix (ownership/nesting guard at the result switch, or serializing ROM-call drivers) is a follow-on plan authored against this repro. **Depends on** the harness `ready_timeout` parameter added by `2026-06-14-landmine6-reproduce.md` Task 1 (slow TSAN boot); Task 1 below re-checks it. See `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md` (Sub-project 2).

---

## File Structure

- **Verify/Modify** `tests/lib/harness.py` — needs the `ready_timeout` param (from the #6 plan). If absent, add it (same diff as the #6 plan Task 1).
- **Create** `tests/phase6/repro_landmine5_two_thread_core.py` — the repro.
- **Artifact (not committed)** `build-tsan/` — refreshed.
- **Modify** `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md` — paste the captured TSAN signature as evidence.

---

### Task 1: Prerequisites — harness timeout + fresh TSAN build

**Files:** `tests/lib/harness.py` (verify), `build-tsan/` (artifact)

- [ ] **Step 1: Confirm the harness accepts `ready_timeout`**

Run: `grep -n "ready_timeout" tests/lib/harness.py`
Expected: a match in `emulator(...)`. If **no** match, apply the #6 plan's Task 1 diff (add `ready_timeout=25` to `emulator()` and pass it to `wait_ready`), then continue.

- [ ] **Step 2: Refresh the TSAN binary**

Run: `cmake --build build-tsan --parallel "$(nproc)" 2>&1 | tail -8`
Expected: successful build; `test -x build-tsan/pose64 && echo OK` → `OK`.

(No commit — build artifact.)

---

### Task 2: Identify the two concurrent CPU-driving paths (the crux)

This race cannot be reproduced until we know which main-thread path runs the 68K core (or stops it on a syscall) concurrently with a worker ROM call. This task is an **investigation**; its output is the concrete driver used in Task 3.

**Files:** read-only survey (no edits to ship)

- [ ] **Step 1: Enumerate CPU-driving call sites by thread**

Run:
```bash
grep -rn "ExecuteSubroutine\|->CallCPU\|ATrap::DoCall\|SuspendThread (kStopOnSysCall\|SuspendThread(kStopOnSysCall" src/core src/ui | sort
```
Expected: a list of callers. Classify each as worker-thread (ReControl handlers in `ReControlCmds_*.cpp`, `CPUWorkerThread.cpp`) or main-thread (`EmApplication`/Qt idle `HandleIdle`, menu handlers, `EmWindow`). Record which main-thread site issues a ROM call or `kStopOnSysCall`.

- [ ] **Step 2: Confirm overlap with temporary instrumentation**

Add a temporary `LogAppendMsg` (or `fprintf(stderr,...)`) at the top and bottom of `EmSession::ExecuteSubroutine` printing the calling thread id and `fNestLevel`, and one in the candidate main-thread site. Build `build-tsan`, run a GUI session on DISPLAY :1, and drive the worker path (`apps`/`launch` in a loop) while exercising the main-thread path; confirm from the log that the two overlap (both inside their ROM-call regions at the same time). **Remove the instrumentation before any commit (R4).**

- [ ] **Step 3: Record the chosen driver**

Write one line into the spec's Sub-project 2 `### Reproduce` block naming the confirmed concurrent main-thread driver, so Task 3's test targets it specifically.

(No commit — investigation; instrumentation is reverted.)

---

### Task 3: Write the repro test

**Files:** Create `tests/phase6/repro_landmine5_two_thread_core.py`

- [ ] **Step 1: Write the test**

Create `tests/phase6/repro_landmine5_two_thread_core.py` (adjust the main-thread driver in `drive()` to Task 2's finding; the worker hammer below is fixed):

```python
#!/usr/bin/env python3
"""Landmine #5 reproduction (recovery-plan R1).

A second SuspendThread(kStopOnSysCall) caller claims a stopped-on-syscall CPU
it did not stop (EmSession.cpp:998-1004; no ownership guard). The window opens
because ExecuteSubroutine releases fSharedLock during CallCPU (:1343-1346), so
a main-thread ROM/idle path overlapping a worker ROM call can leave TWO paths
driving UAE's global `regs` -> corruption.

Method: build-tsan under a REAL X display; hammer the worker ROM-call path
(apps/launch) while the main thread drives its ROM/idle path (see Task 2), and
look for a TSAN data race on the m68k register globals with execution-path
frames from two different threads.

Convention: exit 1 = signature PRESENT (reproduced, RED); exit 0 = absent.
"""

import os
import re
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6438
TSAN_LOG = "/tmp/pose64-landmine5-tsan.log"
DURATION_S = 40
DISPLAY = os.environ.get("DISPLAY", ":1")


def find_signature(text):
    """A data race naming the m68k register state with execution-path frames
    is the #5 signature (two threads in the core), distinct from #6's paint
    family."""
    blocks = re.split(r"(?=WARNING: ThreadSanitizer: data race)", text)
    for b in blocks:
        if "ThreadSanitizer: data race" not in b:
            continue
        names_regs = ("global 'regs'" in b) or ("regs" in b and "EmCPU" in b)
        in_core = ("ExecuteSubroutine" in b or "CallCPU" in b
                   or "EmCPU68K" in b or "EmSession::Execute" in b)
        if names_regs and in_core:
            return b
    return None


def drive(c):
    """Hammer the worker ROM-call path.  Task 2 determines the concurrent
    main-thread driver — add it here (e.g. repeated `menu` activations, or a
    second client exercising the main-thread idle ROM path)."""
    deadline = time.time() + DURATION_S
    i = 0
    while time.time() < deadline:
        c.send_command("apps")            # CollectCurrentAppInfo -> ExecuteSubroutine
        if i % 5 == 0:
            c.send_command("launch Memo Pad")
            c.send_command("launch Launcher")
        i += 1


def main():
    env_extra = {
        "QT_QPA_PLATFORM": "xcb",
        "DISPLAY": DISPLAY,
        "TSAN_OPTIONS": "halt_on_error=0 history_size=7 exitcode=0",
    }
    with emulator(PORT, build="build-tsan", env_extra=env_extra,
                  capture_log=TSAN_LOG, ready_timeout=120) as proc:
        c = connect(PORT, timeout=30)
        try:
            # general churn so the main thread is busy painting/idling too
            c.send_command("gremlin new 42 1000000")
            drive(c)
            c.send_command("gremlin stop")
        finally:
            try:
                c.send_command("quit")
            except Exception:
                pass
            c.disconnect()
        try:
            proc.wait(timeout=20)
        except Exception:
            pass

    with open(TSAN_LOG, "r", errors="replace") as fh:
        text = fh.read()
    total = text.count("WARNING: ThreadSanitizer: data race")
    block = find_signature(text)
    print(f"TSAN data-race reports total: {total}; log: {TSAN_LOG}")
    if block:
        print("REPRO CONFIRMED — landmine #5 two-thread core race present:\n")
        print("\n".join(block.splitlines()[:25]))
        sys.exit(1)
    print("no two-thread core race this run "
          "(refine the main-thread driver per Task 2; try longer DURATION_S)")
    sys.exit(0)


if __name__ == "__main__":
    main()
```

(No run in this task.)

---

### Task 4: Run the repro and confirm it reproduces (RED)

- [ ] **Step 1: Run it**

Run: `python3 tests/phase6/repro_landmine5_two_thread_core.py`
Expected: `REPRO CONFIRMED — landmine #5 two-thread core race present:` and a TSAN block naming `regs` with `ExecuteSubroutine`/`CallCPU`/`EmCPU68K` frames from two threads; exit **1**.

- [ ] **Step 2: If not RED, refine the driver (ties back to Task 2)**

If no two-thread core race appears: the chosen main-thread driver does not overlap reliably. Return to Task 2, pick the next candidate main-thread ROM/`kStopOnSysCall` site, update `drive()` accordingly, and re-run. Increase `DURATION_S` to 80 between attempts. (The race is rare by nature — expect several attempts.)

(No commit until captured.)

---

### Task 5: Record evidence and commit the failing repro (R1 gate)

**Files:** Modify `docs/superpowers/specs/2026-06-14-landmine-hardening-design.md`

- [ ] **Step 1: Capture the signature** — paste the first matching TSAN block from `/tmp/pose64-landmine5-tsan.log` into the spec's Sub-project 2 `### Reproduce` block as `Evidence (captured <date>)` (~20 lines), including the two thread stacks.

- [ ] **Step 2: Verify Unix line endings**

Run: `file tests/phase6/repro_landmine5_two_thread_core.py docs/superpowers/specs/2026-06-14-landmine-hardening-design.md`
Expected: no "CRLF". If any: `sed -i 's/\r$//' <path>`.

- [ ] **Step 3: Commit**

```bash
git add tests/phase6/repro_landmine5_two_thread_core.py docs/superpowers/specs/2026-06-14-landmine-hardening-design.md
git commit -m "test(landmine5): reproduce two-thread 68K core race (RED, R1)

build-tsan under a real display, worker ROM-call hammer overlapping the
main-thread ROM/idle path, surfaces a data race on the m68k register globals
with execution-path frames from two threads. Exit 1 = race present; flips to
exit 0 when the ownership guard lands. Fix is the follow-on plan.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

**Spec coverage:** Implements the spec's Sub-project 2 `### Reproduce` (investigation-first + real-display TSAN hammer + specific two-thread signature + R1 commit). Fix approaches intentionally deferred (Scope).

**Placeholder scan:** Task 2 is an explicit investigation with concrete grep/instrumentation steps — not a hand-wave. The one genuinely-unknown (which main-thread driver) is the named output of Task 2 and the documented refinement loop in Task 4 Step 2, not a silent gap. Test code is complete and runnable as written.

**Type/name consistency:** `find_signature`/`drive`/`TSAN_LOG`/`DURATION_S`/`PORT` consistent; `emulator(..., ready_timeout=...)` matches the #6 plan's harness change; import path `tests.lib.harness` verified.
