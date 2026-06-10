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
