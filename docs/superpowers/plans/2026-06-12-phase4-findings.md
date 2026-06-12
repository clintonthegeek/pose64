# Phase 4 Task 2 — HotSync Handshake Failure: Findings (2026-06-12)

Task 1 outcome B: `tests/phase4/test_hotsync_smoke.py` failed 4 of 5 runs
with pilot-xfer exiting 1 after ~2s: `Error read system info on /dev/pts/N`.
This doc records the root cause (three stacked mechanisms), the
discriminating evidence, the fix, and the acceptance numbers. Method:
superpowers:systematic-debugging — every claim below was measured against
the live system before any fix was written.

---

## Symptom

- 4/5 runs: PTY line appears reliably, pilot-xfer attaches, exits 1 after
  ~2s with `Error read system info`. The script's 15s re-tap fallback never
  fired (pilot-xfer was dead by ~2s).
- 1/5 runs: full pass, 16 databases listed — proving the entire chain
  (cradle tap → guest local sync → UART → PTY → CMP → PADP → DLP) works
  when timing aligns.
- `log set Serial 2` / `log set SerialData 2` produced no output anywhere,
  and `log dump` wrote no `build/Log_*.txt` (separate finding, last
  section).

## Root cause — three stacked mechanisms

### 1. The guest's sync attempt is far shorter than the script assumed

Measured with a raw PTY reader attached immediately after the PTY line
(`/tmp` observation script, hex+timestamps), confirmed UART-side by the
serial log:

- PTY created **+0.55s** after the cradle tap (guest opens port).
- First CMP wakeup **16ms** later; identical 26-byte SLP wakeup frames
  (`BE EF ED 03 03 02 00 0E FF AF | 01 C0 00 0A 01 10 01 03 00 00 00 03
  84 00 | A4 24`, advertised max baud 0x00038400 = 230400) every **~64ms**.
- Volley ends **~1.2s** after the tap (~18 wakeups). Guest closes the
  serial port **~2.4s** after the tap (`EmTransportSerial::Close` at
  log time 1.87s after open).

m515 is uncalibrated (ticks wall-true ≈ 15x device speed), so the
device-side "retries for many seconds" intuition is wrong by an order of
magnitude. The script's attach path (0.2s log-poll granularity + process
spawn + port open) usually lands at or after the end of that window. The
1/5 pass was an attach that happened to land inside it.

### 2. Stale wakeups queue in the PTY and poison a late pilot-xfer

Bytes written to the pty master while **no slave is open** are NOT
discarded — they queue in the slave's input buffer. A late-attaching
pilot-xfer reads the sacrificial volley's ~18 stale wakeups, believes a
live device is present, answers them (PADP ACK + CMP INIT), and moves to
the DLP phase against a dead peer → `Error read system info` ~2s later.
If a fresh tap follows, the real volley reaches a pilot-xfer that is
already past CMP → `Error accepting data` ~0.3s after the tap.

Discriminating evidence (termios probe, two identical runs differing only
in a prior `tcflush`):

- Without flush: while idle (pre-tap), pilot-xfer's slave is already at
  **230400 baud** — the CMP-negotiated rate. It can only know that rate by
  having completed CMP against the stale wakeups.
- With flush: slave still at 9600 (base rate), pilot-xfer still listening
  → subsequent tap syncs.

(A first experiment wrongly concluded "no stale bytes": its drain used
Python `tty.setraw()`, which defaults to `TCSAFLUSH` and silently discards
pending input before the read loop. The drain "read 0 bytes" because
setraw had already flushed them. The 5/5 pass of that experiment was due
to the flush side effect, not the read loop.)

### 3. The modal "HotSync Problem" form swallows re-taps

After a failed attempt the guest shows form id=12000 ("HotSync Problem",
OK button id=12004). `ui` dumps before/after a second `button cradle tap`
are identical — the tap does NOT restart sync while the form is up. So no
blind re-tap strategy can ever work; the form must be dismissed
(`tap-id 12004`) first. (Deterministic-order experiment without dismissal:
0/5, pilot-xfer silent for 60s — no second volley ever transmitted.)

### Residual ~10% phase race (bounded, retried, NOT fixed in the emulator)

Even with the deterministic order below, one acceptance run failed
(`Error accepting data`). Serial logs of a pass and that fail show
**byte-identical traffic**; the only difference is host→UART delivery
latency of pilot-xfer's reply:

- PASS: reply received by comm thread 19ms after port open, delivered
  into the UART RX FIFO **46ms** after the wakeup → guest accepts CMP
  INIT, switches to 230400, DLP completes (~0.9s total).
- FAIL: same packets delivered **56ms** after the wakeup → guest is past
  its per-wakeup listen window (64ms cycle), ignores the INIT, exhausts
  its volley, pops the Problem form again.

The reply itself arrives <1ms after the wakeup; the variable part is the
emulator's RX pump quantum (`EmUARTDragonball` RX pump via session
cycle — the recovery plan's predicted weak spot). At 15x emulated speed a
~50ms host quantum is ~0.75 emulated seconds. This is an emulator timing
weakness worth revisiting (spec 4.3 territory — wall-pacing / RX pump
cadence), but it needs a design decision and is NOT required for GATE 4:
each attempt is independent (~0.9 pass probability measured: 13/14
deterministic attempts), so a bounded retry converges fast.

## Fix (script-level, `tests/phase4/test_hotsync_smoke.py`)

The emulator behavior is left untouched: PTY-on-first-open is faithful
behavior, the PTY persists for the process (`fPtyMaster` cached), and the
deterministic order is available to any client. The smoke script now
encodes the race-free procedure (which becomes the documented contract for
`docs/hotsync.md` in Task 6):

1. `button cradle tap` — sacrificial attempt; its only job is creating
   the PTY.
2. Wait (effect-based, `ui` poll) for the "HotSync Problem" form —
   positive proof the attempt is over and the guest port is closed.
3. `tap-id 12004` to dismiss it.
4. Open the slave once and `tcflush(TCIOFLUSH)` the stale volley.
5. Spawn `pilot-xfer -p <pty> -l`; give it 1s to open the port (the open
   slave also keeps CommRead alive — pty master reads return EIO whenever
   no slave is open).
6. `button cradle tap` — fresh volley goes to a listening, raw-mode
   desktop at base rate; handshake completes in ~1s.
7. Steps 2–6 retry up to 3 attempts (residual phase race above); every
   failed attempt ends in the same Problem form, so the loop precondition
   is uniform.

Also changed: `log set Serial/SerialData` **2 → 1** (see next section), so
the failure path's `log dump` now actually captures byte-level serial
evidence.

## Acceptance

Final committed script, 5 consecutive runs (2026-06-12):

```
=== ACCEPTANCE RUN 1..5 ===
   List complete. 16 files found.
HOTSYNC SMOKE PASS   (x5)
=== 5/5 PASS ===
```

- 5/5 `HOTSYNC SMOKE PASS`, pilot-xfer exit 0, 16 databases each run.
- No orphan processes after each run (`pgrep pilot-xfer` empty;
  only `build/pose64-mcp-proxy` remained).
- `build/.poserrc` restored to `PortSerial=null:` after every run.

Intermediate experiment tallies: racy order 1/5; deterministic without
dismissal 0/5; deterministic with dismissal+flush 5/5, 3/3 (with serial
logging), 1/1; deterministic without flush 0/3; first committed rewrite
(no flush — based on the wrong "no stale bytes" reading) 0/5.

## Why `log set Serial 2` was silent (and `log dump` empty)

Three independent reasons, all verified:

1. **Serial PRINTFs never go to stderr.** `PRINTF` in
   `EmTransportSerialUnix.cpp:37` is `if (!LogSerial()) ; else
   LogAppendMsg` → `LogStream::Printf` → in-memory buffer only. The only
   way out is `log dump` (or exit), which writes `Log_NNNN.txt` next to
   the emulator binary (`EmDirRef::GetEmulatorDirectory()` → `build/`).
2. **The Log* pref value is a BITMASK, not a verbosity level.**
   `EmTypes.h`: `kNormalLogging = 0x01`, `kGremlinLogging = 0x02`. The
   gate (`LogCommon()`, `Logging.h:138`) tests `value & kNormalLogging`
   outside a Gremlin Horde. So `log set Serial 2` means "log ONLY while
   Gremlins run" — silence in a normal run is correct behavior, not a
   broken notification chain (the pref→`gLogCache` notify path was traced
   end-to-end and is intact; `log set Serial 1` verified producing full
   byte-level logs). `RcCmd_Log`'s usage string (`0|1|2`) invites this
   misreading.
3. **`log dump` on an empty buffer writes nothing** —
   `LogStreamInner::DumpToFile` returns early at `fBuffer.size() == 0`,
   creating no file.

Red herring eliminated along the way: a `build/Log_0001.txt` containing a
"Gremlin Hordes started / Gremlin #42" banner predated the smoke run by 2h
(leftover from an earlier landmine-7 stress session — the Hordes banner is
logged unconditionally, and warnings logged there because `2` includes
`kGremlinLogging` and a Horde WAS running). `gremlin status` confirmed no
Horde during smoke runs; m515.psf contains no Horde state.
