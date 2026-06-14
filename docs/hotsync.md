# HotSync against pilot-link (verified 2026-06-12)

POSE64 syncs with pilot-link over a virtual PTY serial port. Verified
end-to-end on 2026-06-12 (m515 session): `pilot-xfer -l` exits 0 and lists
the device's 16 databases. Automated reproduction:

    python3 tests/phase4/test_hotsync_smoke.py

**As of Phase 4.5 the procedure is the natural order — launch, attach
pilot-xfer, tap the cradle once.** Three emulator-side fixes removed the
former order-sensitivity: the PTY now exists from startup (eager creation),
stale bytes are flushed by the guest's port close, and RX delivery is
event-driven so it no longer loses the CMP timing race. The old
"sacrificial tap → dismiss form → flush → attach → tap" dance is no longer
needed; it survives only as troubleshooting below and in this file's git
history. See "Why attach-before-tap (and the Phase 4.5 fixes)".

## Procedure (automated)

`tests/phase4/test_hotsync_smoke.py` launches a fresh emulator
(`build/pose64 -psf m515.psf` plus
`-preference PortSerial=serial:pty:HotSync`), reads the PTY slave path from
`info`, attaches pilot-xfer, taps the cradle once, and asserts:

- `pilot-xfer -p /dev/pts/N -l` exits 0 and its output contains a
  database listing (`Preferences` exists on every Palm OS device; the
  stock m515 session lists 16 databases), and
- the emulator is still `running` afterwards.

A single bounded retry remains as a CI safety net; it is **reported** when
it fires (the run prints `NOTE: needed N attempts`). A retry firing
regularly is a regression — see Troubleshooting.

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
> print nothing locally. Single-line probes (`state`) are usually fine;
> multi-line responses are the unreliable case.

1. **Launch** with `-preference PortSerial=serial:pty:HotSync` (or set the
   GUI preference once). The PTY is created immediately; the slave path is
   in `info` / `palm_state` (`pty=/dev/pts/N`) and on stderr.

       build/pose64 -psf m515.psf --port 6416 \
           -preference PortSerial=serial:pty:HotSync

   (Persistent alternative: GUI Preferences → Serial Port =
   `pty:HotSync`, stored in `.poserrc` next to the binary.)

2. **Attach pilot-xfer**: `pilot-xfer -p /dev/pts/N -l` (give it ~1 s).

3. **Tap the cradle**: `button cradle tap` / `palm_button name=cradle
   action=tap`.

4. The listing prints within ~2 s and the guest returns to `running`.

If an attempt fails anyway (a failed attempt shows the modal "HotSync
Problem" form, id=12000): dismiss it (`tap-id 12004`), re-attach pilot-xfer,
tap again. No flush is needed — the guest's port close discards stale bytes
(Phase 4.5).

## Why attach-before-tap (and the Phase 4.5 fixes)

**The guest's retry window is tiny — so attach first.** m515 has no
throttle calibration, so its ticks run wall-true at ~15× real device
speed. Measured: the first CMP wakeup follows shortly after the cradle tap,
identical 26-byte SLP wakeup frames repeat every ~64 ms for only ~1.2 s
(~18 wakeups), and the guest closes the port ~2.4 s after the tap. An
attach that *starts* after the tap usually lands at or past the end of
that window (1/5 measured pre-fix). **Phase 4.5 fix — eager PTY creation:**
the PTY now exists from transport install (startup), so pilot-xfer can be
attached and listening *before* the first tap; there is no window to miss.

**Stale wakeups used to poison a late pilot-xfer.** Bytes written to a pty
master while no slave is open were not discarded — they queued in the
slave's input buffer. A late-attaching pilot-xfer read a sacrificial
volley, believed a live device was present, completed CMP against the dead
attempt, and died with `Error read system info`. **Phase 4.5 fix —
flush-on-close:** when the guest closes its serial port, the close path
flushes the pty (slave-side `tcflush(TCIOFLUSH)`), so a prior attempt's
unanswered bytes can never reach the next listener. A real serial line does
not buffer for absent readers.

**Delivery-phase timing used to lose ~10% of attempts.** Even in the right
order, pilot-xfer's CMP-init reply reached the host <1 ms after the wakeup
but was delivered into the emulated UART only on the RX pump's
32K-instruction (~50 ms) quantum: 46 ms after the wakeup won, 56 ms missed
the guest's 64 ms per-wakeup listen window (byte-identical traffic).
**Phase 4.5 fix — event-driven RX pump:** the host comm read thread sets a
relaxed atomic the CPU loop checks each cycle, so RX is delivered promptly
instead of on the quantum. Result: the soak test
(`tests/phase4/test_hotsync_soak.py`, 10 single-attempt syncs, no retries)
goes 10/10 (pre-fix ~9/10).

**The "HotSync Problem" form swallows cradle taps.** After a *failed*
attempt the guest shows modal form id=12000; while it is up,
`button cradle tap` does nothing. This is unchanged guest behavior — it
matters only on the recovery path now (dismiss with `tap-id 12004` before
re-tapping), since the happy path no longer produces a failed first
attempt.

Full measured evidence (PTY byte traces, termios probes, serial logs,
experiment tallies, and the Phase 4.5 resolution addendum):
`docs/superpowers/plans/2026-06-12-phase4-findings.md`.

## Device choice

Use a device with NO throttle-calibration entry so Palm OS ticks stay
wall-true (`src/core/EmDeviceBenchmark.h` — only PalmM500 is calibrated,
with its ~2.66× busy-tick skew corrected in the throttle, not the
timer). m515 and Palm Vx are both safe; the verified runs used
`m515.psf`. The wall-true speed is also why the retry window above is an
order of magnitude shorter than real-device intuition suggests.

## Troubleshooting

- **No PTY at startup** — check `info` / `palm_state` shows
  `serial=serial:pty:HotSync pty=/dev/pts/N`. If `serial=` is absent the
  preference did not take (transport still null); if `serial=` is right
  but `pty=` never appears, eager creation did not run — confirm the
  binary is from 2026-06-12 or later (Phase 4.5).
- **`Error read system info` ~2 s after attaching** — should no longer
  occur as of Phase 4.5 (the guest's port close flushes stale bytes). On a
  binary built before 2026-06-12, see this file's git history for the old
  flush-then-attach recovery dance. If seen on a current binary, that is a
  regression — run `tests/phase4/test_pty_stale_flush.py`.
- **`Error accepting data` ~0.3 s after the sync tap** — should no longer
  occur as of Phase 4.5 (the event-driven RX pump eliminated the ~10%
  delivery-phase race). If seen on a current binary, that is a regression —
  run `tests/phase4/test_hotsync_soak.py` (the contract is 10/10).
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
