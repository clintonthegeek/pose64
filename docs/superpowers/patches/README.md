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
