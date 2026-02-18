# Access Violation "Ignore" Feature — Design Notes

## Problem

POSE's access violation dialogs are modal and block emulation. When a Palm app
(or OS component) triggers a violation every frame or during boot, the user
cannot dismiss the dialog fast enough — another one appears immediately.
Examples:

- **SimCity**: reads system globals (0x13E, 0x122, 0xAF4, 0xE98) every frame
  for performance. Legitimate app behavior, technically violates Palm OS
  guidelines, but every real Palm device runs it fine.
- **UIAppShell**: reads system globals (0x102, 0x38E) during boot/app launch.
  This is a RAM-resident OS component that `InRAMOSComponent` fails to
  recognize because its creator may not be in the hardcoded 2001-era list, or
  the database directory isn't ready during early boot.

On the original 32-bit FLTK POSE, these violations almost never manifested due
to timing coincidence (see `docs/meta-check-accessok-bug.md`). The Qt port
exposes the underlying issue: POSE's violation system has no way to suppress
repeat violations or let the user continue without interruption.

## Desired Behavior

1. **Default off**: Access violation reporting should default to disabled.
   The original POSE defaulted to enabled, but this only worked because
   violations rarely fired. On the Qt port they fire frequently. Users who
   want to debug their own Palm apps can enable specific checks.

2. **Per-address/per-PC suppression**: When a violation dialog IS shown, offer
   an "Ignore" button that suppresses future violations from the same PC or
   for the same address. This lets the user acknowledge the first occurrence
   and continue without being interrupted.

3. **Session-scoped ignore list**: Suppressed violations persist for the
   current session only. Reset on app restart or ROM reload.

4. **Bulk ignore**: A "Stop Reporting" button that disables the entire
   category (e.g., all global variable access reports) for the session.

## Implementation Sketch

### Phase 1: Default off (immediate)

Change `PreferenceMgr.h` defaults for the most disruptive checks:

```
ReportLowMemoryAccess       (false)
ReportSystemGlobalAccess    (false)
ReportScreenAccess          (false)
ReportLowStackAccess        (false)
```

This matches the effective behavior of 32-bit POSE (where these never fired).

### Phase 2: Ignore button (future)

Add to the violation dialog:
- **"Ignore (this PC)"**: adds `{PC, address_type}` to a session-scoped
  `std::set<emuptr>` checked in `ProbableCause` before `GetWhatHappened`.
- **"Stop Reporting"**: sets the corresponding `Report*Access` preference to
  false for the session.

### Phase 3: Debug Options UI (future)

Add a Qt dialog equivalent of the original POSE "Debug Options" panel where
users can toggle individual violation checks on/off. This dialog should be
accessible even when a violation dialog is showing.

## Files

| File | Change |
|------|--------|
| `src/core/PreferenceMgr.h` | Change defaults to false |
| `src/core/Hardware/EmBankDRAM.cpp` | Future: check ignore set in ProbableCause |
| `src/core/ErrorHandling.cpp` | Future: add "Ignore" button to dialog |
