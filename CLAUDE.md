All files written to this project MUST use Unix line endings (LF only, no CRLF). After creating or writing any file, verify with `file <path>` and fix with `sed -i 's/\r$//'` if needed.

## Orientation (read before changing anything)

- `docs/STATUS.md` — current, audited truth: what works, known landmines.
- `docs/architecture.md` — threading model and the **Do-Not-Do list**. Read
  it before touching threading, event delivery, ROM calls, or painting.
- `docs/recovery-plan-2026-06.md` — the active roadmap and its binding
  process rules (reproduce-first, replace-don't-stack, effects-not-responses,
  clean tree per session, docs in the same commit). Its **CURRENT POSITION
  banner** (top of file) names the next task and links its detailed plan;
  keep that banner current when you finish a task. The 2026-06-10 plan docs
  under `docs/superpowers/plans/` that it links are ACTIVE, not historical.
- `docs/recontrol-protocol.md` — the ReControl TCP command reference.
- Everything in `docs/history/` is a dated historical record, NOT current.

This project was previously derailed by symptom-layer fixes to cross-thread
bugs and by docs that declared victory before verification. Do not trust a
doc's claim about behavior without checking the code; do not write "X works"
anywhere until you have run it.
