# Phase 3 Design — Unify the MCP + Debugging Control Surface

**Date:** 2026-06-11
**Status:** Approved design (brainstorming complete; next step: writing-plans)
**Supersedes:** recovery-plan Phase 3 as written (scope expanded by user decision —
see "Decision record" below; the recovery-plan banner must be amended when the
implementation plan lands).

## Context

POSE64's agent-facing control apparatus is two generations deep: the original
POSE debugging machinery (DebugMgr, MetaMemory checks, SLP debugger sockets,
Gremlins, the Metrowerks profiler) and the new ReControl/MCP layer built to give
generative agents direct runtime control and introspection. Phase 3 closes the
gap between them — both **fixing the original program** (silent breakpoints, the
SLP stop trap, the MetaMemory freeze) and **finishing the new layer** (the MCP
proxy exposes 27 of 37 useful command surfaces, and its internal tables can
drift).

What exploration found beyond the recovery plan's 3.1–3.4:

- The proxy has **three** drift-prone parallel structures, not two:
  `get_tools_list()` (`pose64-mcp-proxy.cpp:408`), the `dispatch_tool()`
  if-chain (`:557`), and `cmd_is_idempotent()` (`:53`) — the last governs
  reconnect/resend safety, so a misclassified tool can double-execute.
- The proxy **silently defaults missing required arguments** (`palm_tap` with
  no `x` becomes `tap 0 0`) — an honesty violation at the proxy layer, the
  exact sin Phase 2 eliminated server-side.
- `speed` (added in Phase 2) has no MCP tool.
- Task 3.1 as written would expose `palm_check` while landmine #7 (`check set`
  → O(n) heap scan → 100% CPU) is unfixed.

## Decision record (user choices, 2026-06-11)

1. **Scope:** broader MCP-surface rethink — 3.1–3.4 plus the adjacent gaps
   above, re-examining the whole tool surface (naming, granularity, schemas,
   error contract). Session budget grows from 1 to an estimated 2–3 sessions.
2. **Granularity:** tiered — flat everyday tools (one tool per action, proven
   shape) + one tool per debug *group* with an `action` enum.
3. **`break` (3.2):** make it real (dialog on hit), not remove it.
4. **`check` / landmine #7:** fix the O(n) scan at the root (reproduce-first,
   with an evidence-triggered fallback).
5. **Proxy unification:** one static C++ table + a drift-gate test (not
   server-side introspection, not codegen).

## Section A — The tool surface (37 tools)

### Core tools (27)

The existing 28 minus one merge; shapes otherwise unchanged:

- **`palm_dbs` is absorbed into `palm_apps {all?: boolean}`** — both wrapped
  the same server command (`apps` / `apps all`). All MCP consumers are
  in-repo (SKILL.md, pose64-tester, .mcp.json sessions) and are updated in the
  same commit.
- **Enums replace free-text** wherever the vocabulary is fixed:
  `palm_pen.action ∈ {down,up}`;
  `palm_button.name ∈ {power,up,down,app1,app2,app3,app4,cradle,contrast}`,
  `.action ∈ {down,up,tap}`; `palm_reset.type ∈ {soft,hard,debug}`;
  `palm_dialog.respond ∈ {ok,cancel,continue,debug,reset,yes,no}`.
- **Central required-argument validation**: the dispatcher validates each
  tool's `required` list and basic types before building the TCP command;
  a missing argument returns `ERR usage: missing required argument '<name>'`
  with `isError: true`. No silent defaults, ever.
- **Error contract unchanged**: text passthrough of the server's
  `OK`/`ERR <category>:` lines plus MCP `isError`. No structured-JSON
  migration.

### New tools (10)

| Tool | Schema (sketch) | Server command | Notes |
|---|---|---|---|
| `palm_speed` | `{value?: "1".."10000" \| "max"}` | `speed [...]` | omit value = query; server validates range (`ReControlCmds_Session.cpp:698`) |
| `palm_backtrace` | `{}` | `backtrace` | idempotent, multiline; works in `blocked_on_ui` |
| `palm_break` | `{action: list\|set\|clear\|enable\|disable\|clearall, idx?, addr?, condition?}` | `break ...` | `set` requires `idx`+`addr`; see C1 |
| `palm_watch` | `{action: set\|clear\|status, addr?, nbytes?}` | `watch ...` | raises dialog on hit (existing) |
| `palm_spy` | `{action: set\|clear\|status, addr?}` | `spy ...` | raises dialog on change (existing) |
| `palm_log` | `{action: list\|set\|dump\|clear, category?, level? (0\|1\|2)}` | `log ...` | 20 categories |
| `palm_gremlin` | `{action: new\|status\|suspend\|step\|resume\|stop, seed?, events?}` | `gremlin ...` | description warns: input suppressed while running |
| `palm_check` | `{action: list\|set\|set-all\|clearall, flag?, on?: boolean}` | `check ...` | depends on C3; description states the true cost |
| `palm_errorhandling` | `{action: get\|set, setting?, behavior?}` | `errorhandling ...` | |
| `palm_profile` | `{action: init\|start\|stop\|dump\|print\|cleanup\|cycles, max?, depth?, path?}` | `profile ...` | description states init→start→stop→dump ordering |

Action-conditional requirements (e.g. `break set` needs `idx`+`addr`,
`break list` needs nothing) are enforced by each tool's command builder,
returning `ERR usage` on a miss.

**Threading safety — zero new exposure.** Every new tool maps to an existing
ReControl command whose dispatch category was audited in Phase 0–1
(`backtrace` = `kCmdAdaptive`, so it works in `blocked_on_ui`;
`log`/`check`/`errorhandling` = `kCmdImmediate`; `gremlin`/`profile` =
`kCmdWorkerRaw`; `break`/`watch`/`spy` = `kCmdWorkerCycle`). The proxy adds no
threading semantics of its own.

**Truthful descriptions as a design rule.** Each tool description carries its
real contract — the warning lives where the agent reads it, at call time, not
buried in docs.

## Section B — Proxy single-source-of-truth

One static table in `pose64-mcp-proxy.cpp`; `tools/list`, dispatch, and
idempotency all derive from it. Drift between the three is impossible by
construction.

```cpp
struct ToolDef {
    const char* name;         // "palm_break"
    const char* description;  // truthful contract, enums spelled out
    SchemaFn    schema;       // builds inputSchema (properties + required)
    BuildFn     build;        // args -> { tcp_command, multiline, idempotent }
    HandlerFn   custom;       // nullptr unless special (screenshot/state/dialog/ping)
};
static const ToolDef kTools[] = { /* 37 entries */ };
```

Two refinements over the naive sketch, discovered during analysis:

- **Multiline-ness and idempotency are per-action, not per-tool** (`break
  list` is multiline + idempotent; `break set` is neither). The builder
  therefore returns `{command, multiline, idempotent}` — reconnect/resend
  safety stays per-action-honest.
- **Custom handlers stay in the table** as function pointers (`screenshot`'s
  base64/image return, `state`'s state+info merge, `dialog`'s
  query-vs-respond, `ping`) — special cases declared, not scattered.

The single-file, no-Qt, no-thread proxy design is retained.

### Proxy tests (no emulator required)

1. **Drift gate** — `tests/phase3/test_mcp_surface.py` spawns the proxy, sends
   `initialize` + `tools/list` over stdio, and cross-checks tool names and
   parameters against SKILL.md's tool table. Binary and docs can no longer
   disagree silently.
2. **Translation test** — `tests/phase3/test_mcp_dispatch.py` runs a fake
   ReControl server (small Python socket) and asserts each tool call produces
   exactly the expected TCP command string, including `ERR usage` paths for
   missing/invalid arguments.

## Section C — Fixing the original program

### C1. `break` becomes real (task 3.2)

In `Debug::EnterDebugger`'s no-debugger fallback (`DebugMgr.cpp:1219-1222`,
today `result = 1` → the hit is silently ignored), raise the same
deferred-error dialog path `watch`/`spy` use: dialog text
"Breakpoint N hit at 0x…" → `blocked_on_ui`, register dump available through
the existing `dialog` query, resume via the existing `dialog respond continue`.
**No new ReControl command** (R2 — the hit becomes visible through the surface
that already exists).

Agent workflow: `palm_break action=set` → trigger → poll `palm_state` for
`blocked_on_ui` → `palm_dialog` / `palm_backtrace` / `palm_peek` →
`palm_dialog respond=continue`.

This rides the 1.0d-hardened dialog state machine (queued/running/done/
cancelled lifetime). Verification: `tests/phase3/test_break_real.py` sets a
breakpoint, hits it, inspects, continues — 3× pass before STATUS.md landmine
#1 flips to FIXED.

### C2. SLP trap defused (task 3.3)

- Debugger listening sockets (6414/2000, created unconditionally at
  `DebugMgr.cpp:1948` area) **off by default** behind a preference, with a
  `--slp-debugger` CLI flag for humans who want external Palm Debugger attach.
- `Debug::EventCallback`'s untimed `kStopOnSysCall` stop gets the 5000ms
  timeout pattern established by task 1.2.
- Interplay with C1 is clean: the dialog fallback fires only when no SLP
  client is attached — after this change, the normal state. Landmine #8 →
  FIXED (on test evidence).

### C3. MetaMemory O(n) scan — root fix (landmine #7)

Mechanism (read from code, 2026-06-11): `META_CHECK` (`MetaMemory.h:438`
region) fires on every DRAM access; when any DRAM check flag is on it calls
`MetaMemory::InRAMOSComponent` (`MetaMemory.cpp:4083`), which has a one-chunk
cache + tagged-chunk list, but cache misses fall through to
`PrvSearchForCodeChunk` — a walk of every database. The 0bc2a41 evidence
(~200KB/min leak from "repeated heap structure traversal") indicates cache
thrash or unbounded growth, i.e. **cache/invalidation repair, not a rewrite**.

R1 reproduce-first, three steps:

1. **Measure**: harness enables one DRAM check flag, drives sustained activity
   (gremlin), records CPU%, command latency, and RSS over 10+ minutes; `perf`
   identifies where the time goes (database walks vs. invalidation churn vs.
   list growth).
2. **Fix what the profile says** — candidates in likely order: repair the
   tagged-chunk cache invalidation/thrash; or replace per-access database
   walks with an interval map of system-code ranges rebuilt on heap-change
   notifications (`EmPalmHeap` already has the hooks). The `gMetaCheckActive`
   flags-off bypass **stays** (pure win); the fix makes flags-ON usable.
3. **Acceptance**: all six DRAM flags ON, 10-minute gremlin run — no CPU
   runaway, ReControl responsive throughout, RSS flat. Then the protocol doc's
   performance warning is rewritten truthfully (bounded residual overhead, not
   freeze), and landmine #7 → FIXED.

**Timebox + fallback (evidence-triggered, pre-agreed):** C3 is the one
unknown-depth item. If measurement reveals something structurally worse than
cache repair, ship `palm_check` with the truthful freeze warning in its
description, leave #7 listed, and file the root fix as its own task. The
fallback triggers only on evidence, not convenience.

## Section D — Consolidation & docs (task 3.4 + R5)

- `ReControlClient` promoted to a shared module `tests/lib/recontrol_client.py`;
  `test_recontrol.py`, the stress test, and repro harnesses import it.
- `test_cpu_worker_tap.py` → `tests/`, ported onto the client.
- `datebook_interaction.py` and `garak_intrigue.py` (scratch demos with
  hand-rolled TCP) — **deleted**; git history preserves them.
- Same-commit doc updates per change (R5): `docs/recontrol-protocol.md` (debug
  section loses its "Not MCP tools" banner; break semantics rewritten; speed;
  SLP default), `claude/skills/palm-dev/SKILL.md` (new tool table + a
  crash-debugging walkthrough), `claude/agents/pose64-tester.md`,
  `docs/STATUS.md` (landmines #1, #7, #8 re-statused on evidence only),
  `docs/recovery-plan-2026-06.md` banner (scope amendment: Phase 3 expanded to
  this design, 2–3 sessions).

## Section E — Testing & GATE 3 (amended)

`tests/phase3/` contents:

| Test | Verifies |
|---|---|
| `test_mcp_surface.py` | drift gate: tools/list ↔ SKILL.md; schemas well-formed |
| `test_mcp_dispatch.py` | proxy translation + usage-error paths (fake server) |
| `test_break_real.py` | C1: set → hit → `blocked_on_ui` → inspect → continue, 3× |
| C3 harness | landmine #7 measurement + acceptance run |
| SLP repro | C2: off by default; pref on → bounded stop, no UI wedge |

Phase-1/2 repro suites must stay green throughout (regression floor).

**GATE 3 (amended):** a fresh agent given only SKILL.md completes
install → launch → crash → inspect (backtrace via MCP) → recover, **plus**
set-breakpoint → hit → inspect → continue, with zero "Unknown tool" and zero
raw-TCP fallbacks; drift test green; phase-1/2 repros green.

## Sequencing (dependency shape; detail belongs to the implementation plan)

1. Proxy unification + rethought surface (everything else lands tools into the
   unified table) — includes `palm_speed` and the 9 debug groups whose server
   commands already work.
2. C2 (small, de-risks C1's environment).
3. C1 (`break` real) — its tool description and docs flip in the same commit.
4. C3 (the timeboxed unknown) — `palm_check`'s description finalized by the
   outcome.
5. D (consolidation) woven in; docs per-commit per R5.

## Risks

- **C3 depth unknown** → timeboxed, evidence-triggered fallback (above).
- **C1 touches CPU-thread dialog scheduling** → mitigated by the 1.0d state
  machine, a dedicated repro test, and a TSAN spot-check of the new path.
- **Tool renames break consumers** → all consumers in-repo, updated in the
  same commit; drift gate enforces agreement thereafter.
- **Scope creep beyond this design** → the recovery plan's guardrail stands:
  no new MCP features beyond Phase 3 until v1.0.

## Out of scope

- UAE core changes, cycle exactness, multi-client ReControl (recovery-plan
  non-goals).
- Structured-JSON MCP results (text contract retained).
- Server-side tool introspection / codegen (rejected unification approaches).
- Landmines #5, #6, #11 (separate phases/tasks).
