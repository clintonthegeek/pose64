# Capturing Crash Data with GDB

Quick reference for running QtPOSE (or any program) under gdb to capture
backtraces from crashes.

## Batch Mode (Automatic Backtrace on Crash)

This is the most useful mode — gdb runs the program and automatically prints
a backtrace when it crashes, then exits:

```bash
gdb -batch \
  -ex "handle SIGPIPE nostop noprint pass" \
  -ex run \
  -ex "thread apply all bt" \
  -ex "info registers" \
  -ex quit \
  --args ./src/build/qtpose -psf "path/to/SomeProfile.psf"
```

Pipe through `tee` to save output:

```bash
gdb -batch \
  -ex "handle SIGPIPE nostop noprint pass" \
  -ex run \
  -ex "thread apply all bt" \
  -ex "info registers" \
  -ex quit \
  --args ./src/build/qtpose -psf "path/to/SomeProfile.psf" \
  2>&1 | tee /tmp/crash.log
```

### What the flags do

| Flag | Purpose |
|------|---------|
| `-batch` | Non-interactive mode — exits after commands complete |
| `-ex "handle SIGPIPE nostop noprint pass"` | Prevents gdb from stopping on SIGPIPE (common with socket disconnects) |
| `-ex run` | Start the program |
| `-ex "thread apply all bt"` | Print backtrace for ALL threads (critical for multi-threaded programs) |
| `-ex "info registers"` | Dump CPU registers at crash point |
| `-ex quit` | Exit gdb |
| `--args` | Everything after this is the program and its arguments |

## Interactive Mode

For more detailed investigation:

```bash
gdb --args ./src/build/qtpose -psf "path/to/SomeProfile.psf"
```

Then at the `(gdb)` prompt:

```
(gdb) handle SIGPIPE nostop noprint pass
(gdb) run
```

When it crashes:

```
(gdb) bt                        # backtrace of current thread
(gdb) thread apply all bt       # backtrace of ALL threads
(gdb) info registers            # CPU registers
(gdb) frame 3                   # switch to frame #3 in the backtrace
(gdb) info locals               # local variables in current frame
(gdb) print someVariable        # inspect a specific variable
(gdb) quit
```

## Reading a Backtrace

A backtrace looks like:

```
Thread 5 (Thread 0x7fffc9bff6c0 (LWP 1625164) "QThread"):
#0  0x00007ffff5eadfe2 in ??? () at /usr/lib/libc.so.6
#1  0x00007ffff5ea216c in ??? () at /usr/lib/libc.so.6
...
#8  0x00007ffff7bbb278 in QDialog::exec() () at /usr/lib/libQt6Widgets.so.6
#9  0x00005555558b4451 in PrvHostCommonDialog(...)
#10 0x00005555558b4654 in EmDlg::HostRunDialog(void const*) ()
```

Key things to look for:

- **Which thread crashed** — the SIGSEGV thread is the one that faulted
- **Your code frames** — addresses starting with `0x0000555555` are your binary.
  Frames with `???` are in libraries without debug symbols
- **The call chain** — read bottom to top to understand how the crash was reached
- **Thread names** — "QThread" vs main thread tells you which thread context

## Debug Builds

For better backtraces with line numbers, build with debug symbols:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -C build -j$(($(nproc)-1))
```

This gives you file:line info in every frame instead of bare addresses.

## Common Patterns

### Multi-threaded crash (Qt widget on wrong thread)
If you see `QDialog::exec()` or `QWidget` methods on a non-main thread,
that's the bug — Qt widgets must only be used on the main (UI) thread.

### Use-after-free / null dereference
Look for the crashing frame — if it's dereferencing a pointer in your code,
check the pointer value in `info registers` or `print ptr`. `0x0` is null,
small values like `0x8` or `0x48` suggest a null struct pointer + field offset.

### Stack overflow
A backtrace with hundreds of recursive frames or `#0` in the stack guard
page suggests stack overflow.
