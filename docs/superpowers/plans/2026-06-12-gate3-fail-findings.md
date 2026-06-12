# Gate 3 — FAIL Findings (2026-06-12)

## What happened

Gate 3 was attempted 2026-06-12. Pre-flight passed: `test_mcp_surface.py` and
`test_mcp_dispatch.py` both green (37/37). Emulator launched offscreen on
port 6416 with `freshm515.psf`. A `pose64-tester` subagent was dispatched with
the exact Task C3 prompt from plan 3c.

**Pre-gate blocker (resolved before dispatch):** A stale breakpoint at `0x100182BA`
survived from a prior session — breakpoints live in the emulator process, not the
session file, so they persist across `palm_load` and across `palm_reset`. The
address is Memo Pad's ROM event loop, so it fired immediately on every continue.
Recovery sequence: `palm_reset type=hard` (boots to Launcher, bypasses Memo Pad)
→ `palm_break clearall` executed in the window before Memo Pad ran again.

**Gate result: FAIL.** Two gate criteria were not met:
1. Bash fallback used for emulator process restart in steps 5 and 6
2. Breakpoint clearall impossible via MCP alone on hot ROM addresses (step 6)

---

## Gap 1 (BLOCKING): `palm_break clearall` times out on hot ROM addresses

### Symptom

After a breakpoint fires and the CPU is `blocked_on_ui`, the sequence
`palm_dialog respond=continue` → `palm_break clearall` always returns:

```
ERR timeout: CPU did not reach a cycle boundary within 5000ms.
Recovery: dismiss any dialog (dialog respond) or palm_reset.
```

`palm_reset` escapes one level but re-fires during boot through the same ROM
path. The only exit from the cycle is emulator process restart. This blocked
the gate's cleanup of its own breakpoints and forced a Bash fallback.

### Mechanism

`palm_break clearall` is a WorkerCycle command — it waits for a CPU cycle
boundary, which only arrives while the CPU is running. All built-in ROM-app
backtrace frames live in the ROM event-dispatch loop (`EvtGetEvent` and
friends). After `respond=continue`, the CPU runs for nanoseconds and
immediately re-hits the breakpoint before any WorkerCycle boundary is reached.
The loop has no natural window.

### Fix (emulator code change — HIGH PRIORITY)

When the CPU is `blocked_on_ui`, the CPU thread is frozen on the dialog — it
cannot be reading the breakpoint table concurrently. It is therefore safe to
modify the breakpoint table directly without a cycle boundary. The fix: when
`fState == kBlockedOnUI`, process `palm_break clearall/clear/disable` immediately
rather than queueing a WorkerCycle command.

This is the same pattern as task 1.0d for `palm_reset`: 1.0d let `ForceReset`
operate immediately from `blocked_on_ui` by dismissing the dialog first and
bypassing the normal cycle-boundary machinery. Apply that same logic to the
breakpoint-disable path.

This fix also eliminates the pre-gate stale-breakpoint problem: a gate run
can clean up its breakpoints even if the emulator ends up stuck on a dialog.

---

## Gap 2 (BLOCKING): ROM poke is permanent for the session; recovery via `respond=reset` loops

### Symptom

Gate step 3 pokes `0x4AFC` (ILLEGAL) into ROM at `0x100182BA`. The crash fires
as intended. Recovery via `palm_dialog respond=reset` causes the device to reboot
— but the ILLEGAL instruction remains in ROM (POSE64 ROM is debug-writable but
the poke persists for the lifetime of the process). The device boots, Memo Pad
resumes as the active app, executes through `0x100182BA` → crashes again →
`blocked_on_ui` again, indefinitely.

### Fix (gate scenario design — no code change)

Before poking the ILLEGAL instruction, the gate prompt must specify:
1. `palm_peek addr=<target> nbytes=2` to save the original 2 bytes
2. After the crash (while still `blocked_on_ui`), poke the original bytes back
3. Then `palm_dialog respond=continue` — the CPU resumes at the original code

The gate prompt in plan 3c Task C3 must be updated to include these steps.

---

## Gap 3 (quality, not blocking): `palm_load` from `blocked_on_ui` deadlocks

### Symptom

`palm_load path=freshm515.psf` called while in `blocked_on_ui` accepted the
TCP connection and never responded. Required process kill to recover.

### Fix

`palm_load` should detect `blocked_on_ui` and reject immediately with an
actionable error (`ERR blocked: dismiss dialog first with 'dialog respond'`)
rather than deadlocking. This is lower priority — if Gap 1 is fixed the
session load won't be needed for breakpoint cleanup.

---

## Gap 4 (quality, not blocking): `palm_apps` timeout gives no recovery hint

### Symptom

`palm_apps` occasionally returns `ERR timeout: CPU did not reach syscall
boundary within 5000ms` when the Launcher is in its idle loop. Clears after
a soft reset. The error message does not suggest recovery steps.

### Fix

Add a recovery hint to the error message: "Recovery: call palm_state to confirm
running, then retry." Text-only change, no code needed.

---

## What the gate DID confirm (no "Unknown tool")

Every tool call resolved — no "Unknown tool" at any point. These flows are verified:

| Scenario | Result |
|----------|--------|
| `palm_install` with missing file | `ERR usage: file not found` — self-describing |
| `palm_launch "Memo Pad"` + `palm_ui` verification | PASS |
| `palm_poke` to ROM (debug-writable) | WORKS — POSE64 allows ROM writes for debugging |
| `palm_regs` + `palm_backtrace` while running | WORKS |
| `palm_backtrace` while `blocked_on_ui` | WORKS |
| `palm_dialog` query (message + full register dump inline) | WORKS |
| `palm_break set` → trigger → `blocked_on_ui` → inspect | WORKS end-to-end |
| `palm_reset type=hard` from `blocked_on_ui` | WORKS (task 1.0d fix confirmed) |
| No raw-TCP or socat fallbacks | Confirmed — Bash used only for process lifecycle |

---

## Next steps for the fix session

1. **Fix Gap 1 (required for gate pass):** Implement `blocked_on_ui`-immediate
   path for `palm_break clearall/clear/disable`. Pattern: task 1.0d for
   `palm_reset`. When `fState == kBlockedOnUI`, write the breakpoint table
   directly without queueing a WorkerCycle command. Write a repro test first
   (R1): arm a breakpoint on a hot address, trigger it, confirm `clearall`
   succeeds from `blocked_on_ui`, confirm `palm_state` → running after continue.

2. **Fix gate prompt (required for gate pass):** Update Task C3 in plan 3c to
   specify save-before-poke and restore-before-recover. The corrected step 3
   should be: peek original bytes at the target address, poke ILLEGAL, trigger
   crash, inspect crash, poke original bytes back while blocked, then respond=continue.

3. **Fix Gap 3 (quality):** Add blocked-state guard to `palm_load`.

4. **Fix Gap 4 (quality):** Add recovery hint to syscall-boundary timeout error.

5. **Re-run Gate 3** after fixes 1 and 2. Pre-flight: `test_mcp_surface.py` +
   `test_mcp_dispatch.py`. Ensure no stale breakpoints before dispatch.
   On PASS: tag `phase-3-complete`, update STATUS.md and recovery-plan banner.

## Session state on close (2026-06-12)

- Emulator: running on port 6416, `freshm515.psf` loaded, all breakpoints clear
- Git: master @ `837391a` (landmine #7 root fix, last clean commit)
- Working tree: clean
