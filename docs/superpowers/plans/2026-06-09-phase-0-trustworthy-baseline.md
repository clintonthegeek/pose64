# Phase 0 — Freeze a Trustworthy Baseline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to
> implement this plan task-by-task (inline; the tasks share one working tree and
> must run sequentially — do NOT parallelize with subagents). Steps use checkbox
> (`- [ ]`) syntax for tracking.

**Goal:** Turn the abandoned 2026-03-13 working tree into a clean,
buildable-from-fresh-clone repo whose HEAD is trustworthy — instrumentation
gone, the one unverified behavior change deferred (not stranded in the
baseline), reference junk ignored, dead source removed, and `cpp-mcp`
registered so a fresh clone builds.

**Architecture:** The 2026-03-13 changes are all *uncommitted* working-tree
edits. "Stripping instrumentation" therefore means `git restore` (discard),
not a commit. Only genuine keepers and structural fixes produce commits. Each
source-touching commit is followed by a build. The phase ends at GATE 0.

**Tech Stack:** Qt6/C++17, CMake (legacy layout → build dir `build/`), git
submodules, ReControl TCP (port 6416).

**Decisions (resolved with the user before writing this plan):**
- Commits land **directly on master** (consent granted).
- The PuppetString poll-delivery change is **deferred to Phase 2** — saved as a
  patch, then discarded from the baseline.
- `src/cpp-mcp/` enters as a **proper submodule** (hkr04/cpp-mcp @ `dc86c91`);
  GATE 0's clone step is adjusted to init that submodule (from a local
  reference, so it works offline).

**Reviewer-visible defaults chosen for files the recovery plan didn't enumerate:**
- Project artwork (`POSE64_banner.png`, `icon_smaller_no64.png`, `icon_orig.xcf`)
  → **committed** as repo assets (author's own work; README references "my
  artwork"). Veto here if you'd rather gitignore them.
- Scratch Python clients (`datebook_interaction.py`, `garak_intrigue.py`,
  `test_cpu_worker_tap.py`) → **gitignored, kept locally**; Phase 3.4 will
  consolidate them onto `ReControlClient` or delete them.
- `docs/history/` stays **gitignored** (long-standing `.gitignore` policy:
  "kept locally, not published"). The recovery plan's "commit docs/history
  additions" bullet conflicts with this; we honor the existing `.gitignore`.
  Not required for GATE 0.

---

## Task 0: Pre-flight — safety snapshot + baseline build

**Files:** none modified.

- [ ] **Step 1: Snapshot the exact pre-Phase-0 state (recoverable if anything goes wrong)**

```bash
cd /home/clinton/dev/POSE64
git diff HEAD > /tmp/phase-0-preimage.patch
git status --porcelain > /tmp/phase-0-status.txt
git tag -f phase-0-start
wc -l /tmp/phase-0-preimage.patch   # expect ~650+ lines
```

- [ ] **Step 2: Confirm the current (instrumented) tree builds — establishes a known-good baseline**

Run: `cmake --build build -j 2>&1 | tail -20`
Expected: build completes, `build/pose64` and `build/pose64-mcp-proxy` exist.
If it does NOT build: **STOP** and report — the baseline is broken and the plan's
assumptions don't hold.

```bash
ls -la build/pose64 build/pose64-mcp-proxy
```

---

## Task 1: Discard debug instrumentation (no commit — uncommitted changes)

**Files (restore to HEAD):**
- `src/core/CPUWorkerThread.cpp` (pure instrumentation)
- `src/core/EmApplication.cpp` (pure instrumentation)
- `src/core/EmSession.cpp` (pure instrumentation + a comment deletion tied to the deferred work)
- `src/core/ReControl.cpp` (pure instrumentation)
- `src/platform/EmApplicationQt.cpp` (pure instrumentation)
- `src/core/EmWindow.cpp` (pure instrumentation; Wiggle Walk handled in Task 3)

- [ ] **Step 1: Restore the six pure-instrumentation files to HEAD**

```bash
git restore --source=HEAD --worktree -- \
  src/core/CPUWorkerThread.cpp \
  src/core/EmApplication.cpp \
  src/core/EmSession.cpp \
  src/core/ReControl.cpp \
  src/platform/EmApplicationQt.cpp \
  src/core/EmWindow.cpp
```

- [ ] **Step 2: Verify those six are gone from the diff**

Run: `git diff --stat -- src/core/CPUWorkerThread.cpp src/core/EmApplication.cpp src/core/EmSession.cpp src/core/ReControl.cpp src/platform/EmApplicationQt.cpp src/core/EmWindow.cpp`
Expected: **no output** (all match HEAD).
Remaining modified source should now be only `EmPatchMgr.cpp` and
`EmSPISlaveADS784x.cpp`:
Run: `git diff --stat -- src/`
Expected: exactly those two files.

- [ ] **Step 3: Incremental build (still good)**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: success.

(No commit — HEAD never carried this instrumentation.)

---

## Task 2: Defer the PuppetString poll-delivery change to Phase 2

**Files:**
- Create: `docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch`
- Create: `docs/superpowers/patches/README.md`
- Restore: `src/core/Patches/EmPatchMgr.cpp`

- [ ] **Step 1: Save the change as a patch before discarding it**

```bash
mkdir -p docs/superpowers/patches
git diff -- src/core/Patches/EmPatchMgr.cpp \
  > docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch
wc -l docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch  # expect ~60 lines
```

- [ ] **Step 2: Write the patch README so Phase 2 knows what this is**

Create `docs/superpowers/patches/README.md`:

```markdown
# Deferred patches

Working-tree changes intentionally removed from the baseline during Phase 0
because they were unverified, preserved here for the phase that will verify them.

## 2026-03-13-puppetstring-poll-delivery.patch

The candidate "poll-always" event-delivery change from the final 2026-03-13
session (`EmPatchMgr::PuppetString`): after enqueueing a pen/key event it forces
a nil event + `callROM = kSkipROM; return;`, and in interactive mode (no
Gremlins, no playback) sets `clearTimeout = true` unconditionally so the guest
never sleeps on an infinite timeout — compensating for the removed
`PrvWakeUpCPU` wakeup.

**Status:** UNVERIFIED. Deferred to **Phase 2** (recovery plan Task 2.1/2.2,
option A). The baseline must not carry it (STATUS.md landmine #3).

**Caveat:** the diff also contains the `fprintf` debug instrumentation that was
stripped in Phase 0. Phase 2 should extract only the `kSkipROM`/`clearTimeout`
behavior hunks (the ones without `fprintf`), apply against the live
`EvtGetEvent`/`EvtGetPen` patch path, and gate it on the new delivery test
(Task 2.1) run WITH and WITHOUT the change.
```

- [ ] **Step 3: Discard the change from the baseline**

```bash
git restore --source=HEAD --worktree -- src/core/Patches/EmPatchMgr.cpp
git diff --stat -- src/core/Patches/EmPatchMgr.cpp   # expect no output
```

- [ ] **Step 4: Incremental build**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: success (this is the known-good HEAD code).

- [ ] **Step 5: Commit the preserved patch**

```bash
git add docs/superpowers/patches/
git commit -F - <<'EOF'
docs: preserve deferred 2026-03-13 PuppetString poll-delivery change as a patch

The final 2026-03-13 session left an unverified "poll-always" event-delivery
change in EmPatchMgr::PuppetString (kSkipROM after enqueue + unconditional
clearTimeout in interactive mode). Per the recovery plan, an unverified hack
must not be the baseline, so it is removed from the working tree here and
preserved as docs/superpowers/patches/2026-03-13-puppetstring-poll-delivery.patch
for Phase 2 to verify against the new delivery test (Task 2.1) before adopting.

Refs: docs/STATUS.md landmine #3, docs/recovery-plan-2026-06.md Task 2.1/2.2.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
EOF
```

---

## Task 3: Remove the disabled Wiggle Walk zombie code

**Files:**
- Modify: `src/core/EmWindow.cpp` (delete the `if (0) { ... }` Wiggle Walk block)
- Modify: `src/core/EmWindow.h` (remove the `fWiggled` member)

- [ ] **Step 1: Locate the `fWiggled` machinery**

```bash
grep -rn 'fWiggled\|Wiggle Walk' src/
```
Expected: the `if (0)` block + comment in `EmWindow.cpp` (just after
`HandleIdle`'s `HostDrawingEnd ()`), and an `fWiggled` member declaration in
`EmWindow.h`.

- [ ] **Step 2: Delete the Wiggle Walk block in `src/core/EmWindow.cpp`**

Remove the entire block (comment + `if (0) { ... }`), which begins:

```cpp
	// Wiggle Walk — disabled for now. Investigating deadlock with bridge thread.
	if (0)
	{
		const int	kWiggleOffset = 2;
		EmSessionStopper	stopper (gSession, kStopNow);
		...
	}
```

and ends at the closing `}` of that `if (0)` block (the one immediately before
the function's closing brace). Leave the surrounding function intact.

- [ ] **Step 3: Remove the `fWiggled` declaration in `src/core/EmWindow.h`**

Delete the member line (likely `Bool fWiggled;` or `Bool  fWiggled;`). Use the
exact line from Step 1's grep output.

- [ ] **Step 4: Build**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: success (no remaining references to `fWiggled`).

- [ ] **Step 5: Commit**

```bash
git add src/core/EmWindow.cpp src/core/EmWindow.h
git commit -m "refactor: remove disabled Wiggle Walk zombie code (fWiggled)

The 'Wiggle Walk' window-shake feature was permanently disabled behind if(0)
with a note about a bridge-thread deadlock. No zombie code in the baseline.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 4: Keep the ADS784x pen-delivery comment

**Files:**
- `src/core/Hardware/EmSPISlaveADS784x.cpp` (already modified — comment-only keeper)

- [ ] **Step 1: Confirm it is comment-only**

Run: `git diff -- src/core/Hardware/EmSPISlaveADS784x.cpp`
Expected: a 6-line explanatory comment above `result = 0;` in the
`kChannelPenX`/`kChannelPenY` case; no logic change.

- [ ] **Step 2: Commit**

```bash
git add src/core/Hardware/EmSPISlaveADS784x.cpp
git commit -m "docs(src): explain ADS784x returns 0 — pen flows via PuppetString queue

The digitizer channels deliberately report no pen; pen data is delivered through
PuppetString's software queue (EvtEnqueuePenPoint). The hardware pen interrupt
only wakes the CPU from STOP so SysEvGroupWait returns.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 5: Finalize `.gitignore` and drop stray junk

**Files:**
- Modify: `.gitignore`
- Delete (untracked, not ignored): `docs/thing.pdf`, `c:\palm\bigclock\log.txt`,
  `Screenshot_20260218_182122.jpg`

- [ ] **Step 1: Append a scratch-clients section to `.gitignore`**

Add at the end of `.gitignore`:

```
# Scratch Python clients (Phase 3.4 will consolidate onto ReControlClient or remove)
datebook_interaction.py
garak_intrigue.py
test_cpu_worker_tap.py
```

- [ ] **Step 2: Delete the stray junk files**

```bash
rm -f docs/thing.pdf 'c:\palm\bigclock\log.txt' Screenshot_20260218_182122.jpg
```

- [ ] **Step 3: Verify the scratch clients and junk are no longer in `git status`**

Run: `git status --short`
Expected: `datebook_interaction.py`, `garak_intrigue.py`,
`test_cpu_worker_tap.py`, `docs/thing.pdf`, and the `c:\palm...` file no longer
appear. (`.gitignore` shows as modified.)

- [ ] **Step 4: Commit**

```bash
git add .gitignore
git commit -m "chore: ignore scratch Python clients (Phase 3.4); drop stray junk

datebook_interaction.py / garak_intrigue.py / test_cpu_worker_tap.py are
hand-rolled TCP scratch clients kept locally until Phase 3.4 moves them onto
ReControlClient (or deletes them). Removed stray docs/thing.pdf, a Windows-path
log file, and a one-off screenshot.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 6: Commit the doc / config / asset keepers

**Files:**
- Modified keepers: `CLAUDE.md`, `README.md`, `claude/agents/pose64-tester.md`,
  `claude/skills/palm-dev/SKILL.md`,
  `claude/skills/palm-dev/references/builtin-apps.md`,
  `data/ca.vibekoder.pose64.metainfo.xml`, `docs/debugging-guide.md`,
  `docs/qt-port-architectural-review.md`, `docs/recontrol-protocol.md`,
  `docs/stability-findings.md`
- Deleted obsolete docs: `RECONTROL_IMPLEMENTATION.md`, `TEST_RECONTROL.md`,
  `docs/current-state.md`, `docs/palm-launch-debugging.md`
- New docs: `docs/STATUS.md`, `docs/architecture.md`,
  `docs/recovery-plan-2026-06.md`, `docs/debugging-infrastructure.md`,
  `docs/superpowers/plans/2026-03-13-recontrol-debugging-commands.md`,
  `docs/superpowers/plans/2026-06-09-phase-0-trustworthy-baseline.md` (this plan)
- New config: `.mcp.json`
- New assets: `POSE64_banner.png`, `icon_smaller_no64.png`, `icon_orig.xcf`
- Create: `docs/reference-trees.md`

- [ ] **Step 1: Create `docs/reference-trees.md` (the Task 0.2 reference note)**

```markdown
# Local-only reference trees

These large directories are kept on disk for reference but are **gitignored**
(not published). A fresh clone does not include them and does not need them to
build `pose64` or `pose64-mcp-proxy`.

| Path | Size | What it is |
|---|---|---|
| `src/Emulator_Src_3.5/` | 33 MB | Canonical Palm OS Emulator 3.5 source — the port's reference. Obtain from the original POSE 3.5 source distribution. |
| `abandoned/` | 158 MB | Abandoned experiments. |
| `pose32bit/` | 88 MB | Earlier 32-bit tree. |
| `src/fltk-1.1.10/`, `src/fltk-install/` | 50 MB | FLTK build of the original UI; unused by the Qt6 port. |
| `src/core/UAE/gen/` | 1 MB | UAE generator output (regenerated locally). |

`docs/history/` and `docs/ReControlPostMortem/` are likewise gitignored
("kept locally, not published") per `.gitignore`.
```

- [ ] **Step 2: Stage all keepers, deletions, and new files explicitly**

```bash
# modified keepers (deletions of the obsolete docs are picked up by -u below)
git add CLAUDE.md README.md \
  claude/agents/pose64-tester.md \
  claude/skills/palm-dev/SKILL.md \
  claude/skills/palm-dev/references/builtin-apps.md \
  data/ca.vibekoder.pose64.metainfo.xml \
  docs/debugging-guide.md docs/qt-port-architectural-review.md \
  docs/recontrol-protocol.md docs/stability-findings.md
# stage the four obsolete-doc deletions
git rm --quiet RECONTROL_IMPLEMENTATION.md TEST_RECONTROL.md docs/current-state.md docs/palm-launch-debugging.md
# new docs / config / assets
git add docs/STATUS.md docs/architecture.md docs/recovery-plan-2026-06.md \
  docs/debugging-infrastructure.md docs/reference-trees.md \
  docs/superpowers/plans/2026-03-13-recontrol-debugging-commands.md \
  docs/superpowers/plans/2026-06-09-phase-0-trustworthy-baseline.md \
  .mcp.json POSE64_banner.png icon_smaller_no64.png icon_orig.xcf
```

- [ ] **Step 3: Sanity-check what is staged**

Run: `git status --short`
Expected: every line is staged (`A`/`M`/`D` in column 1), and the only remaining
unstaged item is `src/cpp-mcp/` (`??`, handled in Task 7). No stray untracked
files.

- [ ] **Step 4: Commit**

```bash
git commit -m "docs+config: adopt audited doc set, .mcp.json, 0.9.1 notes, artwork

- New authoritative docs: STATUS.md, architecture.md, recovery-plan-2026-06.md,
  debugging-infrastructure.md, reference-trees.md, the Phase 0 plan, and the
  2026-03-13 recontrol-debugging plan.
- Remove obsolete docs superseded by STATUS.md (RECONTROL_IMPLEMENTATION.md,
  TEST_RECONTROL.md, docs/current-state.md, docs/palm-launch-debugging.md).
- README: ReControl/MCP overview. metainfo: 0.9.1 release notes.
- .mcp.json: project MCP server wiring (./build/pose64-mcp-proxy).
- Project artwork (banner + icon + GIMP source).

docs/history/ remains gitignored per existing policy.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 7: Register `cpp-mcp` as a submodule

**Files:**
- Create: `.gitmodules`
- Add: `src/cpp-mcp` gitlink (pinned at `dc86c91`)

- [ ] **Step 1: Confirm the nested clone is clean and at the expected commit**

```bash
git -C src/cpp-mcp rev-parse HEAD          # expect dc86c91...
git -C src/cpp-mcp status --short          # expect clean (no output)
git -C src/cpp-mcp remote get-url origin   # expect https://github.com/hkr04/cpp-mcp.git
```
If the nested tree is dirty: **STOP** and report (we must pin a real upstream commit).

- [ ] **Step 2: Register it as a submodule**

```bash
git submodule add https://github.com/hkr04/cpp-mcp.git src/cpp-mcp
```
If git refuses because `.git/modules/src/cpp-mcp` already exists, retry with
`--force`:
```bash
git submodule add --force https://github.com/hkr04/cpp-mcp.git src/cpp-mcp
```

- [ ] **Step 3: Verify the registration**

```bash
cat .gitmodules                      # expect [submodule "src/cpp-mcp"] url=.../hkr04/cpp-mcp.git
git ls-files --stage src/cpp-mcp     # expect mode 160000 (gitlink) at dc86c91
grep -n 'cpp-mcp' CMakeLists.txt     # confirm the build references src/cpp-mcp/common only (no add_subdirectory pulling googletest)
```
Expected: exactly one CMake reference, `.../src/cpp-mcp/common` — the build
consumes cpp-mcp's source/includes, it does **not** build cpp-mcp's tests, so
its `test/googletest` sub-submodule is irrelevant to the `pose64` build.

- [ ] **Step 4: Build (proves the submodule path is intact)**

Run: `cmake --build build -j 2>&1 | tail -5`
Expected: success.

- [ ] **Step 5: Commit**

```bash
git add .gitmodules src/cpp-mcp
git commit -m "build: register cpp-mcp as a submodule (hkr04/cpp-mcp @ dc86c91)

The MCP proxy build depends on src/cpp-mcp/common, which was an unregistered
nested clone — a fresh clone neither contained nor built it. Pin it as a proper
submodule so the build is reproducible.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 8: Remove audit-verified dead source

**Files (delete, after verifying each is unreferenced):**
- `src/platform/EmWindowUnix.cpp`
- `src/core/omnithread/{mach,nt,null_thread,posix,solaris}.*`
- `src/core/UAE/cpuemu1.c` … `cpuemu8.c`, `src/core/UAE/missing.c`
- `src/core/jpeg_disabled.h`

- [ ] **Step 1: Enumerate the actual files and verify they are unreferenced**

```bash
# list what actually exists
ls src/platform/EmWindowUnix.cpp \
   src/core/omnithread/{mach,nt,null_thread,posix,solaris}.* \
   src/core/UAE/cpuemu[1-8].c src/core/UAE/missing.c \
   src/core/jpeg_disabled.h 2>/dev/null
# verify none are named in the build
grep -rnE 'EmWindowUnix|omnithread/(mach|nt|null_thread|posix|solaris)|cpuemu[1-8]\.c|UAE/missing\.c|jpeg_disabled' CMakeLists.txt
# verify none are #included by surviving sources
grep -rnE 'jpeg_disabled\.h|EmWindowUnix' src/ --include=*.cpp --include=*.h | grep -v 'src/platform/EmWindowUnix.cpp'
```
Expected: the files exist; **no** CMake or `#include` references. Any file that
IS referenced must be **kept** (drop it from the delete list and note it).

- [ ] **Step 2: Delete the verified-unreferenced files**

```bash
git rm src/platform/EmWindowUnix.cpp \
  src/core/omnithread/mach.* src/core/omnithread/nt.* \
  src/core/omnithread/null_thread.* src/core/omnithread/posix.* \
  src/core/omnithread/solaris.* \
  src/core/UAE/cpuemu1.c src/core/UAE/cpuemu2.c src/core/UAE/cpuemu3.c \
  src/core/UAE/cpuemu4.c src/core/UAE/cpuemu5.c src/core/UAE/cpuemu6.c \
  src/core/UAE/cpuemu7.c src/core/UAE/cpuemu8.c src/core/UAE/missing.c \
  src/core/jpeg_disabled.h
```
(Adjust the exact `omnithread/*` extensions to match Step 1's listing.)

- [ ] **Step 3: Build — proves the deletions were truly dead**

Run: `cmake --build build -j 2>&1 | tail -10`
Expected: success. If it fails on a missing file, `git checkout HEAD -- <file>`
to restore that one, drop it from the commit, and note why it was actually live.

- [ ] **Step 4: Commit**

```bash
git commit -m "chore: remove audit-verified dead source

EmWindowUnix.cpp (FLTK/X11 path, unused by the Qt6 port), the unused
omnithread backends (mach/nt/null_thread/posix/solaris), the checked-in UAE
cpuemu1-8.c + missing.c generator outputs, and the jpeg_disabled.h stub — none
referenced by CMake or any surviving include. Build verified after removal.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Task 9: GATE 0 — fresh-clone build, clean status, ROM boot

- [ ] **Step 1: Working tree is clean**

Run: `git status --porcelain`
Expected: **no output**.

- [ ] **Step 2: Fresh clone builds (submodule initialized from a local reference — no network needed)**

```bash
rm -rf /tmp/pose64-clone
git clone . /tmp/pose64-clone
# init the cpp-mcp submodule from the local populated copy (offline-safe;
# real fresh clones use the github URL recorded in .gitmodules)
git -C /tmp/pose64-clone -c submodule."src/cpp-mcp".url="$PWD/src/cpp-mcp" \
  submodule update --init src/cpp-mcp
cmake -S /tmp/pose64-clone -B /tmp/pose64-clone/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/pose64-clone/build -j 2>&1 | tail -15
ls -la /tmp/pose64-clone/build/pose64 /tmp/pose64-clone/build/pose64-mcp-proxy
```
Expected: configure + build succeed; both binaries exist.

- [ ] **Step 3: `pose64` boots a ROM and ReControl answers on 6416**

Launch the locally-built binary headless and confirm the control plane is live.
(Consult `claude/skills/palm-dev/SKILL.md` for the canonical launch + ROM-load
incantation; the offscreen platform avoids needing a display.)

```bash
QT_QPA_PLATFORM=offscreen ./build/pose64 &
POSE_PID=$!
sleep 4
# ROM is gitignored but present locally; load via ReControl, then query state
printf 'load Palm-Vx-4.0-en.rom\n' | timeout 5 nc -q2 localhost 6416 || true
sleep 3
printf 'state\n' | timeout 5 nc -q2 localhost 6416
# expect a state line indicating the session is running
printf 'quit\n' | timeout 5 nc -q2 localhost 6416 || true
kill $POSE_PID 2>/dev/null || true
```
Expected: `state` returns a running/loaded session (not a connection refused /
empty reply). If `load`'s exact syntax differs, use the palm-dev skill's
documented form or the `palm_load` + `palm_state` MCP tools via the proxy.

- [ ] **Step 4: GATE 0 sign-off**

All three must hold:
1. `git status --porcelain` empty ✓
2. fresh clone configures + builds both binaries ✓
3. `pose64` boots a ROM and `state` answers on 6416 ✓

Clean up: `rm -rf /tmp/pose64-clone`. Phase 0 complete → ready for Phase 1.

---

## Self-review checklist (run before executing)

- **Spec coverage** — recovery plan Phase 0 tasks mapped:
  - 0.1 strip instrumentation → Task 1; keep PuppetString candidate → **deferred**
    Task 2; commit keepers (claude docs, metainfo, ADS784x) → Tasks 4 & 6;
    delete Wiggle Walk → Task 3. ✓
  - 0.2 fresh clone works (`git add src/cpp-mcp`, architecture.md, STATUS.md,
    plan) → Tasks 6 & 7 & 9; Emulator_Src_3.5 decision → gitignore (already) +
    `docs/reference-trees.md` (Task 6). ✓
  - 0.3 `.gitignore` appends → already staged + Task 5 scratch section; archive/
    delete big dirs → gitignored, left on disk (non-destructive); SAFE-DELETE
    dead source → Task 8. ✓
  - GATE 0 → Task 9. ✓
- **Placeholder scan** — every step has concrete commands; exact omnithread
  extensions resolved at Task 8 Step 1 (the one deliberately
  verify-then-act point, because the glob must match real files). ✓
- **Consistency** — `phase-0-start` tag, patch path, and reference-trees doc
  names are used identically wherever referenced. ✓

## Big reference directories (NOT deleted by this plan)

`abandoned/` (158 MB), `pose32bit/` (88 MB), `src/fltk-*` (50 MB),
`src/Emulator_Src_3.5/` (33 MB), `src/core/UAE/gen/` are **gitignored and left on
disk** — GATE 0 ("git status empty" + "fresh clone builds") is satisfied without
destroying 330 MB of local reference material. Physically archiving/removing them
is the user's call, made per-directory, outside this plan.
