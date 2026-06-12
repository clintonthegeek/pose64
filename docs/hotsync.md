# HotSync against pilot-link (verified 2026-06-12)

POSE64 syncs with pilot-link over a virtual PTY serial port. Verified
end-to-end on 2026-06-12 (m515 session): `pilot-xfer -l` exits 0 and lists
the device's 16 databases — 5/5 + 3/3 consecutive passes of the procedure
below. Automated reproduction:

    python3 tests/phase4/test_hotsync_smoke.py

The procedure is **order-sensitive**: the naive "tap the cradle, then
attach pilot-xfer" loses a timing race ~80% of the time (measured 1/5).
See "Why this order".

## Procedure (automated)

`tests/phase4/test_hotsync_smoke.py` launches a fresh emulator
(`build/pose64 -psf m515.psf` plus
`-preference PortSerial=serial:pty:HotSync`), runs the deterministic
sequence below — retrying up to 3 attempts for the residual phase race —
and asserts:

- `pilot-xfer -p /dev/pts/N -l` exits 0 and its output contains a
  database listing (`Preferences` exists on every Palm OS device; the
  stock m515 session lists 16 databases), and
- the emulator is still `running` afterwards.

The script's docstring and `sync_attempt()` are the executable form of
this document; if they ever disagree, the script is what passed.

## Procedure (manual / scripted by hand)

ReControl commands (`button`, `ui`, `tap-id`, `info`) are shown; the MCP
equivalents (`palm_button`, `palm_ui`, `palm_tap_id`, `palm_state`)
behave identically.

> **Transport note (GATE 4 finding, 2026-06-12):** drive ReControl over a
> **persistent** TCP connection (`tests/lib/recontrol_client.py`, or the
> MCP tools). One-shot `socat`/`nc` pipes intermittently lose the
> response — a command like `info` or `ui` can execute server-side yet
> print nothing locally, which reads as "the form never appeared" in
> step 4. Single-line probes (`state`) are usually fine; multi-line
> responses are the unreliable case.

1. **Configure the serial transport.** Launch with the CLI flag (takes
   effect same-run since the Task-4 fix):

       build/pose64 -psf m515.psf --port 6416 \
           -preference PortSerial=serial:pty:HotSync

   (Persistent alternative: GUI Preferences → Serial Port =
   `pty:HotSync`, stored in `.poserrc` next to the binary.)

2. **Sacrificial cradle tap.** `button cradle tap` (MCP:
   `palm_button name=cradle action=tap`). The guest starts a local
   HotSync and opens its serial port — that open is what creates the
   PTY (~0.55 s after the tap). Nothing is listening yet, so this
   attempt is *expected to fail*; its only job is creating the PTY,
   which persists for the rest of the emulator process.

3. **Find the PTY slave path.** Either of:
   - ReControl `info` / MCP `palm_state` →
     `serial=serial:pty:HotSync pty=/dev/pts/N` (the `pty=` field
     appears once the PTY exists);
   - the stderr line:
     `SERIAL: PTY created for "pty:HotSync" — connect HotSync tools to: /dev/pts/N`.

4. **Wait for, then dismiss, the "HotSync Problem" form.** Poll `ui`
   until the form (id=12000) is up — positive proof the attempt is over
   and the guest port is closed (arrives within ~5 s). Then
   `tap-id 12004` (the form's OK button; MCP: `palm_tap_id id=12004`).
   Cradle taps are **swallowed** while this form is showing, so this
   step is not optional.

5. **Flush the stale wakeup packets out of the PTY.** The sacrificial
   volley's ~18 CMP wakeup packets queue in the slave's input buffer
   even while no slave is open. Scripted: open the slave and
   `tcflush(fd, TCIOFLUSH)` (see `flush_pty()` in the smoke test).
   Purely manual alternative: skip this step and use the recovery in
   step 7 — a failed pilot-xfer run drains the queue itself.

6. **Attach pilot-xfer FIRST**, to the now-quiet PTY:

       pilot-xfer -p /dev/pts/N -l

   Give it ~1 s to open the port. (The open slave also keeps the
   emulator's read pump alive — reading a pty master returns EIO
   whenever no slave is open.)

7. **Tap the cradle again.** `button cradle tap`. The fresh wakeup
   volley reaches a listening desktop at base rate; the handshake
   completes and the listing prints in ~1 s.

   - **If you skipped step 5**: pilot-xfer fails fast (~2 s after
     attaching) with `Error read system info` — that failed run has
     just drained the stale packets. Re-run pilot-xfer, then re-tap the
     cradle (dismissing the Problem form first if it is up).
   - **~10% of attempts fail anyway** with `Error accepting data`
     (delivery-phase race, see Troubleshooting). Every failed attempt
     ends in the same Problem form, so just repeat steps 4–7.

## Why this order

**The guest's retry window is tiny.** m515 has no throttle calibration,
so its ticks run wall-true at ~15× real device speed. Measured: the PTY
is created ~0.55 s after the cradle tap, the first CMP wakeup follows
16 ms later, identical 26-byte SLP wakeup frames repeat every ~64 ms for
only ~1.2 s (~18 wakeups), and the guest closes the port ~2.4 s after
the tap. An attach that starts after the tap (poll granularity + process
spawn + port open) usually lands at or past the end of that window —
1/5 measured. Attach-before-tap removes the race entirely.

**Stale wakeups poison a late pilot-xfer.** Bytes written to a pty
master while no slave is open are not discarded — they queue in the
slave's input buffer. A late-attaching pilot-xfer reads the sacrificial
volley, believes a live device is present, completes CMP against the
dead attempt (observable: its slave sits at the negotiated 230400 baud
before any fresh tap), and dies with `Error read system info`; a fresh
tap against that already-past-CMP pilot-xfer yields
`Error accepting data`. The queue must be flushed before a clean
attempt — or consumed by a sacrificial pilot-xfer run.

**The "HotSync Problem" form swallows cradle taps.** After every failed
attempt the guest shows modal form id=12000. While it is up,
`button cradle tap` does nothing (UI dumps before/after the tap are
identical; deterministic order *without* dismissal scored 0/5). Dismiss
it with `tap-id 12004` before re-tapping.

Full measured evidence (PTY byte traces, termios probes, serial logs,
experiment tallies): `docs/superpowers/plans/2026-06-12-phase4-findings.md`.

## Device choice

Use a device with NO throttle-calibration entry so Palm OS ticks stay
wall-true (`src/core/EmDeviceBenchmark.h` — only PalmM500 is calibrated,
with its ~2.66× busy-tick skew corrected in the throttle, not the
timer). m515 and Palm Vx are both safe; the verified runs used
`m515.psf`. The wall-true speed is also why the retry window above is an
order of magnitude shorter than real-device intuition suggests.

## Troubleshooting

- **No PTY after the cradle tap** — the guest never opened the port.
  Check `info` / `palm_state` shows `serial=serial:pty:HotSync` (the
  preference took; `serial=` absent means the transport is still
  null). If `serial=` is right but `pty=` never appears, confirm the
  HotSync app actually launched and started a Local sync (`ui` /
  screenshot).
- **`Error read system info` ~2 s after attaching** — pilot-xfer read
  stale queued wakeups (step 5 skipped or flush failed). The failed run
  has drained the queue: re-run pilot-xfer, then re-tap the cradle
  (dismiss the Problem form first if it is up).
- **`Error accepting data` ~0.3 s after the sync tap** — either
  pilot-xfer was already past CMP from stale packets (previous bullet),
  or the residual ~10% delivery-phase race: pilot-xfer's CMP-init reply
  reaches the host <1 ms after the wakeup but is delivered into the
  emulated UART on the RX pump's ~50 ms quantum. Measured: 46 ms after
  the wakeup passes, 56 ms misses the guest's 64 ms per-wakeup listen
  window — identical packet bytes in both runs. Retry steps 4–7 (the
  smoke test retries up to 3 attempts; ~0.9 success per attempt). An
  emulator-side fix (RX pump cadence / wall pacing — recovery-plan spec
  4.3 territory) is explicitly deferred.
- **Serial logging.** `log set Serial 1` + `log set SerialData 1`,
  reproduce, then `log dump`. The `Log*` pref value is a **bitmask, not
  a level** (`EmTypes.h`): 1 = log in normal runs, 2 = log ONLY while a
  Gremlin Horde runs (silence outside a Horde is correct behavior, not
  a broken log), 3 = both. `log dump` writes `Log_NNNN.txt` next to the
  emulator binary (e.g. `build/`) — and writes nothing if the buffer is
  empty.
- **Derate the link**: `PILOTRATE=9600 pilot-xfer -p /dev/pts/N -l`
  caps pilot-link's advertised baud rate.
- **pilot-link protocol internals**: `/usr/include/pi-{cmp,padp,dlp}.h`
  (CMP / PADP / DLP headers).
