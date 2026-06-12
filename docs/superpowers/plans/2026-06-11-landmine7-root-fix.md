# Landmine #7 Root Fix — Per-Site Verdict Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `check set` DRAM-region flags usable (no CPU pin, no RSS growth) by analyzing each violating site once per arming instead of on every access — the root fix that plan 3c deferred (spec §C3), now un-deferred by user decision.

**Architecture:** Two layers. (1) Re-apply plan 3c Task C2 (negative caching in `PrvSearchForCodeChunk` + dedup in `PrvAddTaggedChunk`) — it cures the first-order `InRAMOSComponent` re-walk and is already written/proven. (2) The new root fix: a per-site verdict cache keyed `(PC, meta-bit signature, read/write)` consulted at the top of `EmBankDRAM::ProbableCause`. First occurrence per key runs the full existing `GetWhatHappened`/`AllowForBugs`/report machinery byte-for-byte as today (including the `Report*Access` pref gate inside `ReportErr*`); repeats are counted and suppressed. Invalidation: an atomic generation counter (bumped on any `Report*Access` pref change, `Reset`, `Load`) lazily applied by the CPU thread, plus precise per-chunk erase in `MetaMemory::ChunkUnlocked` (every cacheable PC is anchored to its containing heap chunk). Verdicts whose derivation consulted dynamic context (stack walks, live UI objects, transient patch state, address-conditioned forgiveness) are never cached.

**Tech Stack:** C++ (MetaMemory/EmBankDRAM/Logging), Python 3 tests on `tests/lib` harness.

**Root-cause evidence (2026-06-11, this session + previous):**
- Mechanism: `META_CHECK` fires per DRAM access from a RAM PC; `GetWhatHappened` does a full heap walk (`GWH_ExamineHeap`) and `AllowForBugs` runs ≥3 `In*` predicates per read; `EmFunctionRange::InRange` never caches a non-match and deliberately `Reset()`s ranges found in RAM (`EmPalmFunction.cpp:221-232`), so every predicate re-runs `FindFunctionName` — a 2-byte-step scan across the PC's whole containing chunk. Nothing remembers a verdict; the same PC pays full price per access. Upstream's design assumption ("expensive… about to report an error in a dialog") breaks under continuous auto-continued violations.
- Measured (instrumented run, C2 re-applied, ScreenAccess armed, gremlin seed 42): **3.2M full analyses in ~2 min from ONE PC** (`0x3C85A`, reading system globals `0x13E`/`0x38E` + low-mem `0x78`); verdict mix **100% non-OK, 0% forgiven**; all verdicts *dropped unreported* at `ReportErr*` because only ScreenAccess's pref was armed — the analysis cost is paid even for classes nobody armed. Unique (PC, class) cardinality: **4**.
- RSS growth (~3.1 MB/min unfixed; 270 MB with C2 alone): per-report machinery (`EmEventPlayback::RecordErrorEvent` grows an unbounded in-memory event stream while gremlins record; 3× `LogAppendMsg` + `LogDump()` per report) plus per-access deferred-error churn. Suppression removes all of it after the first occurrence per site.

**Reporting-semantics change (document everywhere):** one report per `(PC, access-kind signature, r/w)` per arming. Meta-bit signatures collide for some classes (globals/MPT/UI-object share `0x01`; memstruct/free/unlocked share `0x03`), so two different violation kinds at one PC with the same signature produce one report. Re-arming (`check clearall` + `check set`) re-reports. This is sanitizer-style dedup; the alternative was the freeze.

---

## File map

| File | Action | Role |
|---|---|---|
| `src/core/MetaMemory.cpp` | Modify | C2 fix (done on branch) + verdict cache + uncacheable marks |
| `src/core/MetaMemory.h` | Modify | verdict-cache API declarations (+ make constants enum public if private) |
| `src/core/Hardware/EmBankDRAM.cpp` | Modify | cache hook in `ProbableCause`; strip `META_ERROR`/`PCAUSE` traces |
| `src/core/EmPalmFunction.cpp` | Modify | strip `GETRANGE` trace |
| `src/core/Patches/EmPatchState.cpp` | Modify | strip `ENTER_MEMMGR`/`EXIT_MEMMGR` traces |
| `src/core/Logging.cpp` | Modify | invalidate verdict cache on pref change |
| `tests/phase3/test_check_suppression.py` | Create | red→green effect test (CPU does not pin when a flag is armed) |
| `tests/phase3/check_perf_harness.py` | Modify | `--emulog` passthrough (done on branch) |
| `src/pose64-mcp-proxy.cpp` | Modify | `palm_check` description → measured truth + once-per-site semantics |
| `docs/recontrol-protocol.md` | Modify | `check` warning → measured truth + semantics |
| `docs/STATUS.md` | Modify | landmine #7 → FIXED; Phase 3 bullet |
| `claude/skills/palm-dev/SKILL.md` | Modify | `palm_check` guidance |
| `docs/recovery-plan-2026-06.md` | Modify | banner; remove the post-v1 deferral item |

Build: `cmake --build build -j$(nproc)`. Working branch: `landmine-7-root-fix` (exists). All measurement/test runs need the machine-local healthy `m515.psf`.

---

### Task 1: Commit the C2 re-application + harness `--emulog` (already in working tree)

**Files:**
- Modify: `src/core/MetaMemory.cpp` (done — verify only)
- Modify: `tests/phase3/check_perf_harness.py` (done — verify only)

- [ ] **Step 1: Verify the diff is exactly C2 + emulog**

Run: `git diff --stat`
Expected: `src/core/MetaMemory.cpp` (~+40), `tests/phase3/check_perf_harness.py` (~+4), `src/core/Hardware/EmBankDRAM.cpp` (~+40, the temporary PCAUSE block — stays uncommitted until Task 2 strips it; use `git add -p` or per-file adds below).

- [ ] **Step 2: Commit (MetaMemory + harness only — NOT EmBankDRAM.cpp)**

```bash
git add src/core/MetaMemory.cpp tests/phase3/check_perf_harness.py
git commit -m "fix(landmine7): re-apply 3c-C2 — negative caching in PrvSearchForCodeChunk + tagged-chunk dedup

Evidence run (instrumented, ScreenAccess armed, gremlin seed 42): with C2
applied the residual freeze is 3.2M GetWhatHappened analyses in ~2min from
ONE PC (0x3C85A), 100% non-OK verdicts, 0% forgiven, all dropped unreported
at ReportErr* (only ScreenAccess pref armed). 4 unique (PC,class) pairs.
Root fix (per-site verdict cache) follows."
```

### Task 2: Strip committed per-access debug instrumentation

These fprintfs are per-access/per-trap costs on the paths being fixed, and they polluted every prior measurement. Startup-only prints (`SKIN:`, the `DRAM:` size line at `EmBankDRAM.cpp:319`) stay.

**Files:**
- Modify: `src/core/Hardware/EmBankDRAM.cpp` (remove `META_ERROR` block + the temporary `PCAUSE` block + `#include <set>`)
- Modify: `src/core/MetaMemory.cpp` (remove `ALLOWBUGS` block, `MetaMemory.cpp:432-453`)
- Modify: `src/core/EmPalmFunction.cpp` (remove `GETRANGE` block, `EmPalmFunction.cpp:282-288`)
- Modify: `src/core/Patches/EmPatchState.cpp` (remove `ENTER_MEMMGR`/`EXIT_MEMMGR` prints at `:734`/`:761` — remove the whole `if`/trace statements, keep surrounding logic untouched)

- [ ] **Step 1: Remove each block listed above.** In `ProbableCause`, after removal the function body must read exactly: compute `whatHappened`, then the `switch`. In `AllowForBugs`, line 432's `static int sAllowBugsTraceCount…` through line 453's closing brace goes; the function then starts with the `if (forRead)` block. In `EmFunctionRange::GetRange`, only the `sGetRangeTraceCount` block goes. In `EmPatchState.cpp`, check the surrounding function — if the `count` variable exists only for the trace, remove it too.

- [ ] **Step 2: Verify no per-access prints remain**

Run: `grep -rn "META_ERROR\|ALLOWBUGS\|GETRANGE\|ENTER_MEMMGR\|EXIT_MEMMGR\|PCAUSE" src/core/ | grep -v Emulator_Src`
Expected: no output.

- [ ] **Step 3: Build**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -3`
Expected: `Built target pose64`.

- [ ] **Step 4: Commit**

```bash
git add src/core/Hardware/EmBankDRAM.cpp src/core/MetaMemory.cpp src/core/EmPalmFunction.cpp src/core/Patches/EmPatchState.cpp
git commit -m "chore(landmine7): strip committed per-access fprintf instrumentation (META_ERROR/ALLOWBUGS/GETRANGE/MEMMGR traces, 870b7ab leftovers)"
```

### Task 3: RED — the freeze-effect test

The acceptance harness's CPU bar compares first-2-min vs last-2-min *within* the flagged phase, so an **instant** pin passes it. This test closes that hole: it compares pre-arm vs post-arm CPU.

**Files:**
- Create: `tests/phase3/test_check_suppression.py`

- [ ] **Step 1: Write the test**

```python
#!/usr/bin/env python3
"""Landmine #7: arming a DRAM check flag must not pin the CPU.

Effect test (pre-arm vs post-arm CPU), closing the acceptance-harness hole
where an INSTANT pin passes its within-flagged-phase drift bar.

Pre-fix: arming SystemGlobalAccess under gremlin load pins the CPU worker
to ~100% instantly (one hot PC re-analyzed per access).  Post-fix: the
site is analyzed once, repeats are suppressed, CPU stays near baseline.

PASS bars:
  * post-arm CPU median <= pre-arm median + 15pp  (two 30s windows)
  * every `state` round-trip < 2s throughout
  * re-arm (clearall + set) does not pin either (invalidation works,
    re-analysis is once-per-site)
Exit 0 = PASS, 1 = FAIL (phase-1 repro convention).
"""

import os
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402
from tests.phase3.check_perf_harness import cpu_seconds, median  # noqa: E402

PORT = 6428


def cpu_window(proc, c, seconds, label):
    """Sample CPU% every 5s for `seconds`; assert state stays responsive."""
    samples = []
    last_cpu, last_t = cpu_seconds(proc.pid), time.time()
    t_end = time.time() + seconds
    while time.time() < t_end:
        time.sleep(5)
        now = time.time()
        cpu = cpu_seconds(proc.pid)
        samples.append(100.0 * (cpu - last_cpu) / (now - last_t))
        last_cpu, last_t = cpu, now
        t0 = time.time()
        r = c.send_command("state")
        lat = time.time() - t0
        assert r is not None and lat < 2.0, \
            f"{label}: state round-trip {lat:.2f}s (r={r!r})"
    m = median(samples)
    print(f"{label}: cpu median {m:.1f}% samples={[round(s,1) for s in samples]}")
    return m


def main():
    failures = []
    with emulator(PORT) as proc:
        c = connect(PORT, timeout=10)
        try:
            r = c.send_command("gremlin new 42 2000000")
            assert r.startswith("OK"), f"gremlin failed: {r}"
            time.sleep(5)

            pre = cpu_window(proc, c, 30, "pre-arm")

            r = c.send_command("check set SystemGlobalAccess on")
            assert r.startswith("OK"), f"check set failed: {r}"
            post = cpu_window(proc, c, 30, "post-arm")
            if post > pre + 15.0:
                failures.append(f"armed: cpu {pre:.1f}% -> {post:.1f}% (pin)")

            r = c.send_command("check clearall")
            assert r.startswith("OK"), f"clearall failed: {r}"
            r = c.send_command("check set SystemGlobalAccess on")
            assert r.startswith("OK"), f"re-arm failed: {r}"
            rearm = cpu_window(proc, c, 30, "re-arm")
            if rearm > pre + 15.0:
                failures.append(f"re-armed: cpu {pre:.1f}% -> {rearm:.1f}% (pin)")

            c.send_command("check clearall")
            c.send_command("gremlin stop")
        finally:
            c.disconnect()

    if failures:
        print("FAIL:", "; ".join(failures))
        sys.exit(1)
    print("PASS")
    sys.exit(0)


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run, expect RED**

Run: `python3 tests/phase3/test_check_suppression.py`
Expected: `FAIL: armed: cpu ~40% -> ~100% (pin)` and exit 1. If it PASSES here, STOP — the reproduction is gone; re-establish it before touching the fix (R1).

- [ ] **Step 3: Commit the red test**

```bash
git add tests/phase3/test_check_suppression.py
git commit -m "test(landmine7): red — arming a check flag pins CPU (pre-arm vs post-arm bar the harness misses)"
```

### Task 4: The fix — per-site verdict cache

**Files:**
- Modify: `src/core/MetaMemory.h` (API + possibly `public:` for the constants enum)
- Modify: `src/core/MetaMemory.cpp` (cache implementation + invalidation + uncacheable marks)
- Modify: `src/core/Hardware/EmBankDRAM.cpp` (`ProbableCause` hook)
- Modify: `src/core/Logging.cpp` (`PrvUpdateMetaCheckActive` bumps the generation)

- [ ] **Step 1: Declare the API in `MetaMemory.h`** (inside `class MetaMemory`, next to the `GetWhatHappened` declaration; `Errors::EAccessType` is already in scope there). Also verify the constants enum at `MetaMemory.h:228` is publicly accessible (used by `EmBankDRAM.cpp` for the signature mask); if it sits in a private section, move/mark it `public:`.

```cpp
		// Landmine #7 root fix: per-site verdict cache for the check-flag
		// machinery.  A "site" is (PC, meta-bit signature, r/w).  First
		// occurrence runs the full GetWhatHappened/report path; repeats
		// are counted and suppressed.  Invalidated by Report*Access pref
		// changes, Reset, Load (generation), and precisely when the PC's
		// containing chunk is unlocked (code unload/move).
		static Bool				LookupCheckVerdict		(emuptr pc, uint8 metaSig, Bool forRead, Errors::EAccessType& verdict);
		static void				StoreCheckVerdict		(emuptr pc, uint8 metaSig, Bool forRead, Errors::EAccessType verdict);
		static void				BeginVerdictAnalysis	(void);
		static void				MarkVerdictUncacheable	(void);
		static void				InvalidateCheckVerdicts	(void);
```

- [ ] **Step 2: Implement in `MetaMemory.cpp`** (file-scope statics near `gTaggedChunks`; add `#include <atomic>` and `#include <map>` to the includes if absent):

```cpp
// ---------------------------------------------------------------------------
// Landmine #7: per-site verdict cache.  All mutation of gCheckVerdicts
// happens on the CPU thread (ProbableCause / ChunkUnlocked / lazy clear);
// other threads request invalidation by bumping gVerdictGenRequested.
// ---------------------------------------------------------------------------

struct EmCheckVerdictKey
{
	emuptr	pc;
	uint8	metaSig;
	Bool	forRead;

	bool operator< (const EmCheckVerdictKey& o) const
	{
		if (pc != o.pc)				return pc < o.pc;
		if (metaSig != o.metaSig)	return metaSig < o.metaSig;
		return forRead < o.forRead;
	}
};

struct EmCheckVerdictEntry
{
	Errors::EAccessType	verdict;
	emuptr				chunkStart;		// containing chunk body, for ChunkUnlocked
	emuptr				chunkEnd;
	uint64				hits;
};

typedef std::map<EmCheckVerdictKey, EmCheckVerdictEntry>	EmCheckVerdictMap;

static EmCheckVerdictMap		gCheckVerdicts;
static std::atomic<uint32_t>	gVerdictGenRequested (0);
static uint32_t					gVerdictGenApplied;		// CPU thread only
static Bool						gVerdictCacheable;		// CPU thread only
static const size_t				kMaxVerdictEntries = 512;


Bool MetaMemory::LookupCheckVerdict (emuptr pc, uint8 metaSig, Bool forRead, Errors::EAccessType& verdict)
{
	uint32_t	gen = gVerdictGenRequested.load (std::memory_order_acquire);
	if (gen != gVerdictGenApplied)
	{
		gCheckVerdicts.clear ();
		gVerdictGenApplied = gen;
	}

	EmCheckVerdictKey	key = { pc, metaSig, forRead };
	EmCheckVerdictMap::iterator	iter = gCheckVerdicts.find (key);
	if (iter == gCheckVerdicts.end ())
		return false;

	iter->second.hits++;
	verdict = iter->second.verdict;
	return true;
}


void MetaMemory::BeginVerdictAnalysis (void)
{
	gVerdictCacheable = true;
}


void MetaMemory::MarkVerdictUncacheable (void)
{
	gVerdictCacheable = false;
}


void MetaMemory::StoreCheckVerdict (emuptr pc, uint8 metaSig, Bool forRead, Errors::EAccessType verdict)
{
	if (!gVerdictCacheable)
		return;

	// Only PCs anchored to a known heap chunk are cacheable: the chunk
	// range is what lets ChunkUnlocked invalidate this entry when the
	// code unloads or moves.  (META_CHECK only fires for RAM PCs, so
	// ROM PCs never get here.)

	const EmPalmHeap*	heap = EmPalmHeap::GetHeapByPtr ((MemPtr)(uintptr_t) pc);
	if (!heap)
		return;

	const EmPalmChunk*	chunk = heap->GetChunkBodyContaining (pc);
	if (!chunk)
		return;

	if (gCheckVerdicts.size () >= kMaxVerdictEntries)
		gCheckVerdicts.clear ();	// pathological cardinality; start over

	EmCheckVerdictKey	key = { pc, metaSig, forRead };
	EmCheckVerdictEntry	entry = { verdict, chunk->BodyStart (), chunk->BodyEnd (), 0 };
	gCheckVerdicts[key] = entry;
}


void MetaMemory::InvalidateCheckVerdicts (void)
{
	gVerdictGenRequested.fetch_add (1, std::memory_order_release);
}
```

(Verify `EmPalmChunk::BodyEnd()` exists in `EmPalmHeap.h` alongside `BodyStart`/`BodyContains` at `:323-327`; if the accessor has a different name, use that.)

- [ ] **Step 3: Wire invalidation.**

In `MetaMemory::Reset` (`MetaMemory.cpp:88`) and `MetaMemory::Load` (`:125`), next to `gTaggedChunks.clear ()`:

```cpp
	MetaMemory::InvalidateCheckVerdicts ();
```

In `MetaMemory::ChunkUnlocked` (`:3909`), after the existing tagged-chunk erase loop, add a precise erase (CPU thread):

```cpp
	// Landmine #7: drop cached verdicts for sites inside this chunk —
	// the code identity at those PCs is no longer guaranteed.

	EmCheckVerdictMap::iterator	viter = gCheckVerdicts.begin ();
	while (viter != gCheckVerdicts.end ())
	{
		if (addr >= viter->second.chunkStart && addr < viter->second.chunkEnd)
			viter = gCheckVerdicts.erase (viter);
		else
			++viter;
	}
```

In `Logging.cpp` `PrvUpdateMetaCheckActive` (`:45`), after computing `gMetaCheckActive` (this runs on any `Report*Access` pref change — arming/clearing flags re-opens analysis):

```cpp
	MetaMemory::InvalidateCheckVerdicts ();
```

(`Logging.cpp` already includes `MetaMemory.h` for `gMetaCheckActive`; verify, add the include if not.)

- [ ] **Step 4: Hook `ProbableCause`** (`EmBankDRAM.cpp:672`; after Task 2 the function is back to upstream shape). Replace the body's verdict computation:

```cpp
void EmBankDRAM::ProbableCause (emuptr address, long size, Bool forRead)
{
	EmAssert (gSession);

	// Landmine #7 root fix: a site that has already been analyzed is not
	// analyzed again — GetWhatHappened/AllowForBugs cost whole-chunk code
	// scans and heap walks PER ACCESS, which pinned the CPU when a hot
	// loop touched checked memory (measured: one PC, 3.2M analyses in
	// ~2 min).  First occurrence per (PC, access-kind, r/w) behaves
	// exactly as before — including the Report*Access pref gate inside
	// ReportErr* — repeats are counted and suppressed.  Re-arming via
	// check set/clearall invalidates and re-reports.

	emuptr	pc      = gCPU->GetPC ();
	uint8	metaSig = (uint8) (*(EmMemGetMetaAddress (address)) &
				(MetaMemory::kAccessBitMask | MetaMemory::kScreenBuffer | MetaMemory::kStackBuffer));

	Errors::EAccessType	whatHappened;

	if (MetaMemory::LookupCheckVerdict (pc, metaSig, forRead, whatHappened))
		return;

	MetaMemory::BeginVerdictAnalysis ();

	whatHappened = MetaMemory::GetWhatHappened (address, size, forRead);

	MetaMemory::StoreCheckVerdict (pc, metaSig, forRead, whatHappened);

	switch (whatHappened)
	{
		// ... existing switch UNCHANGED ...
	}
}
```

Note the `kUnknownAccess`/`kLowStackAccess` arm of the switch calls `GetWhatHappened` again after `EmAssert (false)` — leave it; it is debug-only re-entry.

- [ ] **Step 5: Mark dynamic-context verdicts uncacheable.** One `MetaMemory::MarkVerdictUncacheable ();` line immediately before each forgiveness `return`/`goto HideBug` whose condition consulted anything beyond the PC and session-constant patch flags. Exact sites in `MetaMemory.cpp` (line numbers pre-Task-2-strip; re-locate by the cited code):

1. `GetWhatHappened` — the `inUIObject` → `kOKAccess` path (`:387-394`): depends on live UI-object lists (`CheckUIObjectAccess`).
2. `AllowForBugs` — the `SecPrvRandomSeed` block (`:466-477`): walks the A6 stack frame.
3. `AllowForBugs` — `IsInSysBinarySearch` (`:529-533`): transient patch state.
4. `AllowForBugs` — `TsmGlueGetFepGlobals` (`:574-579`), `IntlMgrGlobalsP` (`:583-588`), `testHarnessGlobalsP` (`:592-595`): forgiveness conditioned on the *address*, which is not part of the cache key.
5. `GWH_CheckChunk` — the `HasDeletedStackBug` A7 stack-scan for `cj_kptkdelete` (`:1380-1395`, the `while (a7 …) EmMemGet32 (a7)` loop): stack contents.
6. `GWH_CheckChunk` — the `MemMove`-caller checks: `DmWrite`/`FindSaveFindStr` (`:1427-1432`) and `FntDefineFont` (`:1440-1444`): walk A6 frames.
7. While editing GWH: any other forgiveness path that reads CPU registers (`GetRegister`, `GetSP`, `a6`/`a7` chains) or `EmPatchState::IsIn*` transient state gets the same one-liner. PC-range-only checks (`::InPrvCompressedInnerBitBlt ()` etc.) and session-constant `Has*Bug()` ROM-version flags stay cacheable.

- [ ] **Step 6: Build**

Run: `cmake --build build -j$(nproc) 2>&1 | tail -3`
Expected: `Built target pose64`.

- [ ] **Step 7: GREEN — run the suppression test 3×**

Run: `for i in 1 2 3; do python3 tests/phase3/test_check_suppression.py || echo "RUN $i FAILED"; done`
Expected: `PASS` ×3, no `RUN n FAILED`. The post-arm and re-arm medians should sit within a few points of pre-arm (~40%).

- [ ] **Step 8: Commit**

```bash
git add src/core/MetaMemory.h src/core/MetaMemory.cpp src/core/Hardware/EmBankDRAM.cpp src/core/Logging.cpp
git commit -m "fix(landmine7): per-site verdict cache — analyze each (PC,kind,rw) once per arming, suppress+count repeats

Invalidation: atomic generation (pref change/Reset/Load, lazily applied on
CPU thread) + precise erase in ChunkUnlocked (entries are chunk-anchored;
PCs outside any heap chunk are never cached).  Dynamic-context forgiveness
(stack walks, live UI objects, transient patch state, address-conditioned)
is never cached.  test_check_suppression.py 3x PASS."
```

### Task 5: Acceptance — the spec §C2 bars

- [ ] **Step 1: Single flag, 10 min**

Run: `python3 tests/phase3/check_perf_harness.py --acceptance --flags ScreenAccess --minutes 10`
Expected: `ACCEPTANCE PASS` (CPU flat within +10pp first→last 2 min, every probe < 2 s, RSS growth < 10 MB). Record the printed summary numbers.

- [ ] **Step 2: All six DRAM flags, 10 min**

Run: `python3 tests/phase3/check_perf_harness.py --acceptance --flags all --minutes 10`
Expected: `ACCEPTANCE PASS`. Record numbers.

If either FAILS: re-profile (`perf record -F 99 -g -p $(pgrep -f 'build/pose64.*6427') -- sleep 60` during the flagged phase), iterate at most twice, else revert per §C3 and stop — do not stack patches (R2).

### Task 6: Regression sweep (flags-off behavior unchanged)

- [ ] **Step 1: Full sweep**

```bash
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
python3 tests/phase2/test_honest_ack.py
python3 tests/phase3/test_mcp_surface.py && python3 tests/phase3/test_mcp_dispatch.py
python3 tests/phase3/repro_slp_trap.py && python3 tests/phase3/test_break_real.py
```

Expected: all PASS, no `FAILED:` lines. The cache adds zero work when flags are off (`gMetaCheckActive` short-circuits `META_CHECK` before `ProbableCause`).

### Task 7: Truthful docs (same-session, before merge)

- [ ] **Step 1: `src/pose64-mcp-proxy.cpp`** — replace `palm_check`'s landmine-7 warning description with the measured truth, e.g.: `"DRAM-region flags are usable: each violating site (PC + access kind) is analyzed and reported once per arming, repeats are suppressed (re-arm via action=clearall + set to re-report). Measured overhead under gremlin load: <X>pp CPU (Phase 3c root fix)."` Fill `<X>` from Task 5. Rebuild: `cmake --build build -j$(nproc) --target pose64-mcp-proxy` (verify target name with `cmake --build build --target help | grep -i proxy` if it fails).
- [ ] **Step 2: `docs/recontrol-protocol.md`** — rewrite the `check` performance-warning blockquote: measured numbers + the once-per-site-per-arming reporting semantics + signature-collision caveat.
- [ ] **Step 3: `docs/STATUS.md`** — landmine #7 → `~~…~~ **FIXED (landmine-7 root fix, 2026-06-11)**`: keep the measured history, add mechanism (verdict cache, invalidation paths, uncacheable carve-outs) and evidence (suppression test 3×, both acceptance runs' numbers, regression sweep). Update the Phase 3 bullet (3c root fix DONE; GATE 3 still pending).
- [ ] **Step 4: `claude/skills/palm-dev/SKILL.md`** — `palm_check` row/guidance to match the new contract. Then `python3 tests/phase3/test_mcp_surface.py` (drift gate) — expected PASS.
- [ ] **Step 5: `docs/recovery-plan-2026-06.md`** — CURRENT POSITION banner: plan 3c root fix LANDED (un-deferred 2026-06-11, user decision); remove/annotate the named post-v1 task in "explicitly NOT doing"; GATE 3 remains the NEXT ACTION.
- [ ] **Step 6: Commit**

```bash
git add src/pose64-mcp-proxy.cpp docs/recontrol-protocol.md docs/STATUS.md claude/skills/palm-dev/SKILL.md docs/recovery-plan-2026-06.md docs/superpowers/plans/2026-06-11-landmine7-root-fix.md
git commit -m "docs(landmine7): check flags usable — measured acceptance numbers, once-per-site reporting contract"
```

### Task 8: Merge + push

- [ ] **Step 1: Fast-forward master**

```bash
git checkout master && git merge --ff-only landmine-7-root-fix
```

Expected: fast-forward (branch is linear). If not FF, stop and rebase-review.

- [ ] **Step 2: Push**

```bash
git push
```

GATE 3 (fresh-agent MCP run) stays the next action per the banner — run it from a fresh session with the reconnected MCP server, per plan 3c Task C3.
