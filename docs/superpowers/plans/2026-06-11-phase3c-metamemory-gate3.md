# Phase 3c — MetaMemory Root Fix + GATE 3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix landmine #7 at the root (the `check set` O(n)-per-access freeze), finalize `palm_check`'s contract, run the amended GATE 3, and close Phase 3.

**Architecture:** R1 reproduce-first: a measurement harness quantifies the degradation, `perf` names the hot path, and only then does a fix land. Primary hypothesis (from code reading, to be confirmed by the profile): `MetaMemory::InRAMOSComponent` (`MetaMemory.cpp:4083`) caches positive lookups in `gTaggedChunks`, but a PC outside every resource database caches NOTHING — `PrvSearchForCodeChunk` (`MetaMemory.cpp:3992`) then walks all databases × all resources on EVERY DRAM access from that PC. The fix shape: negative-result caching through the same tagged-chunk machinery, plus dedup on insert. An evidence-triggered fallback is pre-agreed (spec §C3): if the profile shows something structurally worse, ship `palm_check` with its truthful warning and file the root fix separately.

**Tech Stack:** C++ (MetaMemory), Python 3 measurement harness, `perf`, pose64-tester agent for GATE 3.

**Spec:** `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`
**Prerequisites:** Plans 3a + 3b complete (GATE 3 needs the full tool surface and real `break`).
**Sequence:** plan 3 of 3 (3a → 3b → **3c**).

---

## File map

| File | Action | Role |
|---|---|---|
| `tests/phase3/check_perf_harness.py` | Create | measurement + acceptance harness (landmine #7) |
| `src/core/MetaMemory.cpp` | Modify | the fix (`PrvSearchForCodeChunk` :3992-4075, `PrvAddTaggedChunk` :3870-3877, `Reset`/`Load` :90/:127) |
| `src/pose64-mcp-proxy.cpp` | Modify | finalize `palm_check` description |
| `docs/recontrol-protocol.md` | Modify | rewrite the performance warning with measured numbers |
| `docs/STATUS.md` | Modify | landmine #7 re-status; Phase 3 completion bullet; GATE 3 record |
| `claude/skills/palm-dev/SKILL.md` | Modify | `palm_check` guidance; GATE 3 is run from this file alone |
| `docs/recovery-plan-2026-06.md` | Modify | GATE 3 PASSED + banner → Phase 4 |

Build: `cmake --build build -j$(nproc)`. The harness needs a healthy machine-local `m515.psf` (see STATUS.md "Session-file baseline" — re-create if missing: boot ROM → calibrate → save).

---

### Task C1: measure the freeze (R1 — reproduce before touching code)

- [ ] **Step 1: Write the harness**

`tests/phase3/check_perf_harness.py`:

```python
#!/usr/bin/env python3
"""Landmine #7 measurement + acceptance harness.

Phases:
  baseline   3 min, all check flags off, gremlin running
  flagged    N min (default 10), DRAM check flag(s) on, same gremlin load

Samples every 10s: emulator CPU%% (from /proc/<pid>/stat deltas), RSS,
and ReControl `state` round-trip latency.  Writes CSV to --csv (default
/tmp/check_perf.csv) and prints a summary.

Modes:
  (default)      measurement only — prints numbers, exits 0
  --acceptance   asserts the spec bars and exits nonzero on failure:
                   * CPU%%: median of last 2 min <= median of first 2 min + 10
                   * every latency sample < 2.0 s
                   * RSS growth across flagged phase < 10 MB
  --flags X,Y    which flags to enable (default ScreenAccess; acceptance runs
                 use all six DRAM flags)
  --minutes N    flagged-phase length (default 10)
"""

import argparse
import csv
import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402

PORT = 6427
DRAM_FLAGS = ["LowMemoryAccess", "SystemGlobalAccess", "ScreenAccess",
              "MemMgrDataAccess", "FreeChunkAccess", "UnlockedChunkAccess"]
CLK_TCK = os.sysconf("SC_CLK_TCK")


def cpu_seconds(pid):
    with open(f"/proc/{pid}/stat") as f:
        parts = f.read().split()
    return (int(parts[13]) + int(parts[14])) / CLK_TCK


def rss_mb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024.0
    return 0.0


def sample_phase(proc, c, label, minutes, rows):
    t_end = time.time() + minutes * 60
    last_cpu, last_t = cpu_seconds(proc.pid), time.time()
    while time.time() < t_end:
        time.sleep(10)
        now = time.time()
        cpu = cpu_seconds(proc.pid)
        pct = 100.0 * (cpu - last_cpu) / (now - last_t)
        last_cpu, last_t = cpu, now
        t0 = time.time()
        r = c.send_command("state") or "TIMEOUT"
        lat = time.time() - t0
        rows.append({"phase": label, "t": round(now, 1), "cpu_pct": round(pct, 2),
                     "rss_mb": round(rss_mb(proc.pid), 1), "lat_s": round(lat, 3),
                     "state": r.strip()[:40]})
        print(rows[-1])


def median(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--acceptance", action="store_true")
    ap.add_argument("--flags", default="ScreenAccess")
    ap.add_argument("--minutes", type=int, default=10)
    ap.add_argument("--csv", default="/tmp/check_perf.csv")
    args = ap.parse_args()
    flags = DRAM_FLAGS if args.flags == "all" else args.flags.split(",")

    rows = []
    with emulator(PORT) as proc:
        c = connect(PORT, timeout=10)
        try:
            r = c.send_command("gremlin new 42 2000000")
            assert r.startswith("OK"), f"gremlin failed: {r}"
            sample_phase(proc, c, "baseline", 3, rows)
            for fl in flags:
                r = c.send_command(f"check set {fl} on")
                assert r.startswith("OK"), f"check set {fl} failed: {r}"
            sample_phase(proc, c, "flagged", args.minutes, rows)
            c.send_command("check clearall")
            c.send_command("gremlin stop")
        finally:
            c.disconnect()

    with open(args.csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)

    flagged = [r for r in rows if r["phase"] == "flagged"]
    first2 = [r["cpu_pct"] for r in flagged if r["t"] <= flagged[0]["t"] + 120]
    last2 = [r["cpu_pct"] for r in flagged if r["t"] >= flagged[-1]["t"] - 120]
    lat_max = max(r["lat_s"] for r in flagged)
    rss_growth = flagged[-1]["rss_mb"] - flagged[0]["rss_mb"]
    print(f"\nflags={flags}")
    print(f"cpu%% first2min median={median(first2):.1f} last2min median={median(last2):.1f}")
    print(f"latency max={lat_max:.3f}s  rss growth={rss_growth:.1f}MB  csv={args.csv}")

    if args.acceptance:
        ok = (median(last2) <= median(first2) + 10
              and lat_max < 2.0 and rss_growth < 10.0)
        print("ACCEPTANCE", "PASS" if ok else "FAIL")
        sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run the measurement on the UNFIXED build (R1 reproduction)**

```bash
python3 tests/phase3/check_perf_harness.py --flags ScreenAccess --minutes 10
```

Expected (per landmine #7): flagged-phase CPU% climbs toward 100 and/or `state` latency degrades markedly vs baseline; possibly RSS growth (the historical ~200KB/min leak). **Record the printed summary numbers — they go into STATUS.md and the commit message.** If 10 minutes shows NO degradation, re-run with `--minutes 20` and `--flags all`; if still clean, STOP — the landmine description is stale; update STATUS.md with the measurement instead and skip to Task C3 (GATE 3), leaving `palm_check`'s warning softened to match reality.

- [ ] **Step 3: Profile the degraded window**

While a second measurement run is in its flagged phase (run Step 2's command in one terminal):

```bash
pid=$(pgrep -f 'build/pose64.*6427')
perf record -F 99 -g -p "$pid" -o /tmp/check_perf.data -- sleep 60
perf report -i /tmp/check_perf.data --stdio --no-children | head -60
```

Expected: hot frames in the `MetaMemory` family — the hypothesis says `PrvSearchForCodeChunk` / `PrvGetRAMDatabaseDirectory` / `EmPalmHeap::GetHeapByPtr` dominate. **Save the top-20 lines.** Decision gate:
- Hot path = the search/cache family → proceed to Task C2 (the planned fix).
- Hot path is something structurally different → **fallback** (spec §C3): skip to Task C2-FALLBACK.

- [ ] **Step 4: Commit the harness + findings**

```bash
git add tests/phase3/check_perf_harness.py
git commit -m "test(phase3c): landmine-7 measurement harness — unfixed baseline: <CPU first->last medians, latency max, RSS growth, top perf frame>"
```

(Fill the angle brackets with the actual measured numbers.)

---

### Task C2: the fix — negative caching + insert dedup

Only execute after Task C1 Step 3 confirms the hot path. The two changes are independent; land both, they are each ~15 lines.

- [ ] **Step 1: Negative-result caching in `PrvSearchForCodeChunk`**

At the END of `PrvSearchForCodeChunk` (`MetaMemory.cpp`, after the database loop falls through without returning), add:

```cpp
	// Phase 3c (landmine #7): the PC is not in ANY resource database (e.g.
	// code running from a locked dynamic-heap chunk, or a patch stub).  The
	// old code cached nothing here, so EVERY subsequent DRAM access from
	// this PC re-walked all databases above — the O(n)-per-access freeze.
	// Cache the containing heap chunk as a non-system tagged chunk instead;
	// chunk moves/frees invalidate it through the same Resync/ChunkUnlocked
	// lifecycle as positive entries.

	const EmPalmHeap*	heap = EmPalmHeap::GetHeapByPtr ((MemPtr)(uintptr_t) pc);
	if (heap)
	{
		const EmPalmChunk*	chunk = heap->GetChunkBodyContaining (pc);
		if (chunk)
		{
			gHaveLastChunk	= true;
			gLastChunk		= EmTaggedPalmChunk (*chunk, false /* not system code */);

			::PrvAddTaggedChunk (gLastChunk);
		}
	}
```

(APIs verified 2026-06-11: `EmPalmHeap::GetChunkBodyContaining(emuptr)` at `EmPalmHeap.h:143`, `EmPalmChunk::BodyStart/BodyContains` at `:323-327`, and the `EmTaggedPalmChunk(const EmPalmChunk&, Bool)` construction pattern matches the existing positive-cache call at `MetaMemory.cpp:4068-4071`.)

- [ ] **Step 2: Insert dedup in `PrvAddTaggedChunk`**

Replace the body (`MetaMemory.cpp:3870-3877`):

```cpp
static void PrvAddTaggedChunk (const EmTaggedPalmChunk& chunk)
{
	// Phase 3c: replace any stale entry covering the same body range instead
	// of appending a duplicate — unbounded growth here was the historical
	// ~200KB/min leak when chunks moved (the old entry was never reclaimed).

	EmTaggedPalmChunkList::iterator	iter = gTaggedChunks.begin ();
	while (iter != gTaggedChunks.end ())
	{
		if (iter->BodyContains (chunk.BodyStart ()))
		{
			*iter = chunk;
			return;
		}
		++iter;
	}

	gTaggedChunks.push_back (chunk);
}
```

(`EmPalmChunk::BodyStart()` verified at `EmPalmHeap.h:323`.)

- [ ] **Step 3: Build, then acceptance — one flag, then all six**

```bash
cmake --build build -j$(nproc)
python3 tests/phase3/check_perf_harness.py --acceptance --flags ScreenAccess --minutes 10
python3 tests/phase3/check_perf_harness.py --acceptance --flags all --minutes 10
```

Expected: `ACCEPTANCE PASS` on both (CPU flat within +10pp, every probe < 2s, RSS growth < 10MB). If FAIL: re-profile (C1 Step 3), iterate the fix at most twice; still failing → revert the fix commits and take Task C2-FALLBACK (evidence trigger, spec §C3).

- [ ] **Step 4: Flags-off regression sweep**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
python3 tests/phase2/test_honest_ack.py
python3 tests/phase3/test_break_real.py
python3 tests/phase3/repro_slp_trap.py
```

Expected: all PASS — the fix only adds work on the previously-pathological miss path; `gMetaCheckActive` still short-circuits everything when flags are off.

- [ ] **Step 5: Truthful docs + description finalization (same commit)**

- `src/pose64-mcp-proxy.cpp` — `palm_check` description's warning becomes the measured truth, e.g.: `"Enabling DRAM-region flags costs roughly <X>% extra CPU under load (measured Phase 3c) — bounded, no freeze. Use action=clearall when done."` Rebuild the proxy.
- `docs/recontrol-protocol.md` — replace the "Performance warning" blockquote with the measured overhead statement (keep the advice to enable briefly).
- `docs/STATUS.md` — landmine #7 → `~~...~~ **FIXED (Phase 3c)**`: mechanism (negative-result caching via the tagged-chunk machinery + insert dedup in `PrvAddTaggedChunk`), evidence (acceptance numbers, both runs).
- `claude/skills/palm-dev/SKILL.md` — `palm_check` row/guidance updated to match.

- [ ] **Step 6: Verify drift gate, commit**

```bash
python3 tests/phase3/test_mcp_surface.py
git add src/core/MetaMemory.cpp src/pose64-mcp-proxy.cpp docs/recontrol-protocol.md \
        docs/STATUS.md claude/skills/palm-dev/SKILL.md
git commit -m "fix(phase3c): MetaMemory negative caching + tagged-chunk dedup — check flags usable (landmine #7)"
```

### Task C2-FALLBACK (only on the evidence trigger from C1/C2)

- [ ] Keep `palm_check`'s landmine-7 warning description exactly as Plan 3a wrote it; revert any failed fix attempts (`git revert`, clean tree per R6).
- [ ] `docs/STATUS.md` landmine #7: append what the profile showed (hot path, numbers) and why the fix was deferred; file the root fix as a named post-v1.0 task in the recovery plan's "explicitly NOT doing (yet)" list.
- [ ] Commit: `docs(phase3c): landmine-7 root fix deferred on profile evidence — <one-line reason>`.

---

### Task C3: GATE 3 — fresh-agent run from SKILL.md alone

- [ ] **Step 1: Pre-flight**

```bash
python3 tests/phase3/test_mcp_surface.py && python3 tests/phase3/test_mcp_dispatch.py
QT_QPA_PLATFORM=offscreen build/pose64 -psf m515.psf --port 6416 &
sleep 8
```

(GATE runs against the default port so the standard `.mcp.json` proxy config applies.)

- [ ] **Step 2: Dispatch the gate agent**

Dispatch a `pose64-tester` subagent with EXACTLY this prompt (the gate's "given only SKILL.md" condition — the agent gets the task, not coaching).
*(Prompt revised 2026-06-12 per the GATE 3 FAIL findings, Gap 2: steps 3 and 5 now save the original bytes before the ILLEGAL poke and restore them while blocked — a ROM poke is permanent for the emulator process, so `respond=reset` without restoring loops the crash forever.)*

```
GATE 3 verification run. Using only the palm_* MCP tools as documented in
claude/skills/palm-dev/SKILL.md (read it first; do NOT use raw TCP, socat,
or any Bash fallback for emulator interaction), complete this scenario
against the already-running emulator:

1. Install /tmp/gate3/DOESNOTEXIST.prc — observe the error, then recover by
   listing apps to confirm the emulator is healthy.
2. Launch the Memo Pad application and verify via the UI structure that it
   is frontmost.
3. Cause a guest crash: pick a code address from palm_backtrace frame 0,
   save its original bytes with palm_peek (addr, nbytes=2) — the poke is
   permanent for the emulator process, so you MUST be able to restore it —
   then poke 2 bytes of 0x4AFC (ILLEGAL) over it, tap the screen to drive
   execution through it, and wait for state=blocked_on_ui.
4. Inspect the crash: capture the dialog message, the register dump, and a
   backtrace.
5. Recover: while still blocked_on_ui, poke the saved original bytes back
   over the target address (peek/poke work while blocked), then respond to
   the dialog with continue, and verify state=running. (Do NOT respond with
   reset while the ILLEGAL bytes are still in place — the device reboots
   into the same crash, indefinitely.)
6. Breakpoint flow: launch Memo Pad again if needed; set a breakpoint on a
   backtrace frame PC, tap to trigger it, confirm the breakpoint dialog
   appears (blocked_on_ui), capture backtrace while stopped, clear all
   breakpoints, continue, and verify state=running.

Report: each step's outcome, every tool call that returned an error and
whether the error message told you what to do instead, any moment you
wanted a tool that did not exist, and any 'Unknown tool' response (these
fail the gate).
```

- [ ] **Step 3: Judge the gate**

PASS requires: all 6 steps completed; zero "Unknown tool"; zero raw-TCP/Bash fallbacks for emulator interaction; the breakpoint flow worked end-to-end. The agent's friction notes (errors that didn't guide, missing tools) get recorded but don't fail the gate unless they blocked a step. FAIL → fix the gap (usually SKILL.md wording or a tool description), commit, re-run the gate fresh.

- [ ] **Step 4: Kill the emulator, record the gate**

```bash
kill %1
```

Record in `docs/recovery-plan-2026-06.md` (GATE 3 line → PASSED with date + scenario summary) and `docs/STATUS.md` (Phase 3 bullet → COMPLETE, GATE 3 PASSED, with the evidence list: surface tests, dispatch tests, repro_slp_trap 3×, test_break_real 3×, check acceptance numbers or documented fallback, gate transcript summary).

---

### Task C4: Phase 3 close-out

- [ ] **Step 1: Banner + status finalization**

`docs/recovery-plan-2026-06.md` CURRENT POSITION banner → **Phase 3 COMPLETE (GATE 3 PASSED <date>)**, next = Phase 4 (HotSync smoke test, task 4.1). `docs/STATUS.md` date line gains "Phase 3 COMPLETE <date>".

- [ ] **Step 2: Final full sweep**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
python3 tests/phase2/test_honest_ack.py
python3 tests/phase3/test_mcp_surface.py && python3 tests/phase3/test_mcp_dispatch.py
python3 tests/phase3/repro_slp_trap.py && python3 tests/phase3/test_break_real.py
```

Expected: all PASS. Anything red blocks the tag (no declared victories without verification).

- [ ] **Step 3: Tag and push**

```bash
git add docs/recovery-plan-2026-06.md docs/STATUS.md
git commit -m "docs(phase3): GATE 3 PASSED — Phase 3 complete; next Phase 4 (HotSync)"
git tag phase-3-complete
git push && git push --tags
```
