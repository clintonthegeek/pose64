# Phase 3a — MCP Surface Rebuild Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rebuild `pose64-mcp-proxy` around a single source-of-truth tool table exposing the approved 37-tool surface with central argument validation, and consolidate the Python test clients.

**Architecture:** One static `kTools[]` array in the proxy drives `tools/list`, dispatch, and reconnect/idempotency policy (replacing three drift-prone parallel structures). Per-action multiline/idempotency flags are computed by each tool's command builder. Python test infrastructure moves into a `tests/lib` package; a drift-gate test pins the proxy's surface to SKILL.md.

**Tech Stack:** C++17 (POSIX sockets + nlohmann/json, single file, no Qt), Python 3 (stdlib only) for tests, CMake target `pose64-mcp-proxy`.

**Spec:** `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`
**Sequence:** This is plan 1 of 3 (3a → 3b `…phase3b-debugger-fixes.md` → 3c `…phase3c-metamemory-gate3.md`).

**Honesty rule carried through every task (R5):** tool descriptions and docs must be true *at each commit*. In this plan `palm_break`'s description still carries the "passive without SLP debugger" warning (landmine #1 — Plan 3b removes it) and `palm_check`'s carries the freeze warning (landmine #7 — Plan 3c finalizes it).

---

## File map

| File | Action | Role |
|---|---|---|
| `tests/__init__.py`, `tests/lib/__init__.py`, `tests/phase3/__init__.py` | Create | packaging |
| `tests/lib/recontrol_client.py` | Create | `ReControlClient` moved verbatim from `test_recontrol.py:19-84` |
| `tests/lib/harness.py` | Create | emulator launcher promoted from `tests/phase1/_harness.py` (+ `extra_args`) |
| `tests/lib/fake_recontrol.py` | Create | scriptable fake ReControl TCP server |
| `tests/lib/mcp_proxy.py` | Create | spawn proxy binary, speak JSON-RPC over stdio |
| `tests/phase3/test_mcp_surface.py` | Create | surface shape + SKILL.md drift gate |
| `tests/phase3/test_mcp_dispatch.py` | Create | exact TCP translation + usage errors (fake server) |
| `src/pose64-mcp-proxy.cpp` | Modify | the rebuild (delete `cmd_is_idempotent` :53-62, `get_tools_list` :408-535, `dispatch_tool` :557-723) |
| `test_recontrol.py` | Modify | import client from `tests.lib`; keep its smoke-test main |
| `tests/phase1/_harness.py` | Modify | re-export from `tests.lib.harness` |
| `tests/test_cpu_worker_tap.py` | Create (move) | ported onto `ReControlClient` |
| `test_cpu_worker_tap.py`, `datebook_interaction.py`, `garak_intrigue.py` | Delete | scratch scripts (git history preserves) |
| `claude/skills/palm-dev/SKILL.md` | Modify | tool table (same commit as each surface change) |
| `docs/recontrol-protocol.md` | Modify | MCP coverage notes (same commit) |
| `claude/agents/pose64-tester.md`, `docs/STATUS.md`, `docs/recovery-plan-2026-06.md` | Modify | close-out task |

Build commands (legacy build dir per CLAUDE.md): `cmake --build build --target pose64-mcp-proxy -j$(nproc)`. Run Python tests from repo root: `python3 tests/phase3/test_mcp_surface.py`.

---

### Task A1: tests/lib package — promote client and harness

**Files:**
- Create: `tests/__init__.py`, `tests/lib/__init__.py`, `tests/lib/recontrol_client.py`, `tests/lib/harness.py`
- Modify: `test_recontrol.py`, `tests/phase1/_harness.py`

- [ ] **Step 1: Create package inits**

```bash
touch tests/__init__.py tests/lib/__init__.py
```

- [ ] **Step 2: Move `ReControlClient` verbatim**

Create `tests/lib/recontrol_client.py` containing exactly the `ReControlClient` class cut from `test_recontrol.py:19-84` (the class is unchanged — this is a relocation), preceded by this header:

```python
#!/usr/bin/env python3
"""ReControl TCP client — the one shared client for every test and script.

Moved here from test_recontrol.py (Phase 3a consolidation, recovery-plan
task 3.4). Import as:  from tests.lib.recontrol_client import ReControlClient
"""

import socket
```

(Carry over any other imports the class body needs — it uses only `socket`.)

- [ ] **Step 3: Re-point `test_recontrol.py`**

In `test_recontrol.py`, delete the class body (lines 19-84) and replace with:

```python
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tests.lib.recontrol_client import ReControlClient  # noqa: E402,F401  (re-export)
```

Keep everything else (the `test_*` functions and `main()`) untouched — they reference `ReControlClient` by name, which the re-export satisfies, and external callers doing `from test_recontrol import ReControlClient` keep working.

- [ ] **Step 4: Promote the harness**

Create `tests/lib/harness.py` with the full contents of `tests/phase1/_harness.py`, with two changes:

1. The import becomes `from tests.lib.recontrol_client import ReControlClient` (the `REPO` sys.path line stays; `REPO` is now `dirname(dirname(dirname(abspath(__file__))))` — same expression, same depth, still correct from `tests/lib/`).
2. `emulator()` gains an `extra_args` parameter (Plan 3b needs `--slp-debugger`):

```python
@contextlib.contextmanager
def emulator(port, psf="m515.psf", build="build", env_extra=None, capture_log=None,
             extra_args=None):
    """Launch pose64 on `port`, yield the Popen handle, guarantee teardown."""
    exe = os.path.join(REPO, build, "pose64")
    if not os.path.isfile(exe):
        raise RuntimeError(f"executable not found: {exe}")

    env = dict(os.environ)
    env.setdefault("QT_QPA_PLATFORM", "offscreen")
    if env_extra:
        env.update(env_extra)

    cmd = [exe, "-psf", os.path.join(REPO, psf), "--port", str(port)]
    if extra_args:
        cmd += list(extra_args)

    log = open(capture_log, "w") if capture_log else subprocess.DEVNULL
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env)
```

(rest of the function body unchanged from `_harness.py`).

- [ ] **Step 5: Shim `tests/phase1/_harness.py`**

Replace the entire file body (keep the docstring) with:

```python
#!/usr/bin/env python3
"""Phase 1 reproduction harness — now a re-export of tests/lib/harness.py
(Phase 3a consolidation). The phase-1 repros keep importing from here."""

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.harness import wait_ready, emulator, connect  # noqa: E402,F401
from tests.lib.recontrol_client import ReControlClient  # noqa: E402,F401
```

- [ ] **Step 6: Verify imports compile and a fast repro still passes**

```bash
python3 -m py_compile tests/lib/recontrol_client.py tests/lib/harness.py \
    tests/phase1/_harness.py test_recontrol.py
python3 tests/phase1/repro_1_8_argval.py
```

Expected: py_compile silent; repro prints PASS (it self-launches an offscreen emulator; requires `build/pose64` + `m515.psf` present).

- [ ] **Step 7: Commit**

```bash
git add tests/__init__.py tests/lib/ tests/phase1/_harness.py test_recontrol.py
git commit -m "refactor(phase3a): promote ReControlClient + harness into tests/lib (task 3.4 start)"
```

---

### Task A2: fake ReControl server + MCP stdio helper

**Files:**
- Create: `tests/lib/fake_recontrol.py`, `tests/lib/mcp_proxy.py`

- [ ] **Step 1: Write the fake server**

`tests/lib/fake_recontrol.py`:

```python
#!/usr/bin/env python3
"""Scriptable fake ReControl server for proxy translation tests.

Listens on an ephemeral port, records every command line received, and
replies according to a script of (prefix, response) rules:

  - response str            -> single line + "\n"
  - response list[str]      -> each line + "\n", then ".\n" (multiline)
  - response ("close",)     -> drop the connection (no reply)

Unmatched commands get "OK\n".
"""

import socket
import threading


class FakeReControl:
    def __init__(self):
        self._rules = []          # list of (prefix, response)
        self.received = []        # every command line, in order
        self._srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._srv.bind(("127.0.0.1", 0))
        self._srv.listen(4)
        self.port = self._srv.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def script(self, prefix, response):
        """Add a rule: commands starting with `prefix` get `response`."""
        self._rules.append((prefix, response))

    def _respond(self, conn, line):
        for prefix, response in self._rules:
            if line.startswith(prefix):
                if isinstance(response, tuple) and response[0] == "close":
                    raise ConnectionAbortedError  # handled in _serve: close conn
                if isinstance(response, list):
                    for ln in response:
                        conn.sendall((ln + "\n").encode("latin-1"))
                    conn.sendall(b".\n")
                else:
                    conn.sendall((response + "\n").encode("latin-1"))
                return
        conn.sendall(b"OK\n")

    def _serve(self):
        self._srv.settimeout(0.2)
        while not self._stop:
            try:
                conn, _ = self._srv.accept()
            except socket.timeout:
                continue
            try:
                buf = b""
                while not self._stop:
                    data = conn.recv(4096)
                    if not data:
                        break
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        cmd = line.decode("latin-1").rstrip("\r")
                        self.received.append(cmd)
                        self._respond(conn, cmd)
            except ConnectionAbortedError:
                pass
            except OSError:
                pass
            finally:
                conn.close()

    def close(self):
        self._stop = True
        self._thread.join(timeout=2)
        self._srv.close()
```

- [ ] **Step 2: Write the MCP stdio helper**

`tests/lib/mcp_proxy.py`:

```python
#!/usr/bin/env python3
"""Drive a pose64-mcp-proxy process over stdio JSON-RPC for tests."""

import json
import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class McpProxy:
    def __init__(self, port, binary=None, recv_timeout=5):
        binary = binary or os.path.join(REPO, "build", "pose64-mcp-proxy")
        self.proc = subprocess.Popen(
            [binary, "--port", str(port), "--recv-timeout", str(recv_timeout)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True)
        self._id = 0
        self.request("initialize", {})

    def request(self, method, params):
        self._id += 1
        msg = {"jsonrpc": "2.0", "id": self._id, "method": method, "params": params}
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("proxy closed stdout")
        return json.loads(line)

    def tools_list(self):
        return self.request("tools/list", {})["result"]["tools"]

    def call(self, name, arguments=None):
        return self.request("tools/call",
                            {"name": name, "arguments": arguments or {}})["result"]

    @staticmethod
    def text(result):
        return "".join(c["text"] for c in result["content"] if c["type"] == "text")

    @staticmethod
    def is_error(result):
        return result.get("isError", False)

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)
```

- [ ] **Step 3: Sanity-run both against the CURRENT proxy binary**

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, ".")
from tests.lib.fake_recontrol import FakeReControl
from tests.lib.mcp_proxy import McpProxy
f = FakeReControl()
f.script("state", "OK running")
p = McpProxy(f.port)
tools = p.tools_list()
print("tools:", len(tools))
r = p.call("palm_ping")
print("ping:", McpProxy.text(r))
p.close(); f.close()
assert len(tools) == 28 and McpProxy.text(r) == "pong"
print("PASS")
EOF
```

Expected: `tools: 28`, `ping: pong`, `PASS` (the old 28-tool surface — proves the helpers work before the rebuild).

- [ ] **Step 4: Commit**

```bash
git add tests/lib/fake_recontrol.py tests/lib/mcp_proxy.py
git commit -m "test(phase3a): fake ReControl server + MCP stdio test helper"
```

---

### Task A3: surface and dispatch tests (red)

**Files:**
- Create: `tests/phase3/__init__.py`, `tests/phase3/test_mcp_surface.py`, `tests/phase3/test_mcp_dispatch.py`

- [ ] **Step 1: Write the surface test**

`tests/phase3/test_mcp_surface.py`:

```python
#!/usr/bin/env python3
"""Phase 3a surface tests: tool catalog shape + SKILL.md drift gate.

No emulator needed — tools/list is served from the proxy's static table.
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.fake_recontrol import FakeReControl  # noqa: E402
from tests.lib.mcp_proxy import McpProxy  # noqa: E402

CORE_TOOLS = [
    "palm_ping", "palm_state", "palm_ui", "palm_apps", "palm_tap",
    "palm_tap_id", "palm_pen", "palm_key", "palm_type", "palm_button",
    "palm_screenshot", "palm_screen_hash", "palm_launch", "palm_install",
    "palm_export", "palm_save", "palm_load", "palm_reset", "palm_sleep",
    "palm_quit", "palm_dialog", "palm_run", "palm_peek", "palm_poke",
    "palm_regs", "palm_menu", "palm_delete",
]
NEW_TOOLS = [
    "palm_speed", "palm_backtrace", "palm_break", "palm_watch", "palm_spy",
    "palm_log", "palm_gremlin", "palm_check", "palm_errorhandling",
    "palm_profile",
]
EXPECTED = CORE_TOOLS + NEW_TOOLS  # 37

ENUMS = {  # tool -> param -> required enum values
    "palm_pen": {"action": {"down", "up"}},
    "palm_button": {"name": {"power", "up", "down", "app1", "app2", "app3",
                             "app4", "cradle", "contrast"},
                    "action": {"down", "up", "tap"}},
    "palm_reset": {"type": {"soft", "hard", "debug"}},
    "palm_dialog": {"respond": {"ok", "cancel", "continue", "debug", "reset",
                                "yes", "no"}},
}


def get_tools():
    f = FakeReControl()
    p = McpProxy(f.port)
    tools = {t["name"]: t for t in p.tools_list()}
    p.close()
    f.close()
    return tools


def test_core_surface(tools):
    missing = [t for t in CORE_TOOLS if t not in tools]
    assert not missing, f"missing core tools: {missing}"
    assert "palm_dbs" not in tools, "palm_dbs must be absorbed into palm_apps {all}"
    apps = tools["palm_apps"]["inputSchema"]
    assert "all" in apps.get("properties", {}), "palm_apps needs the 'all' param"
    for name, t in tools.items():
        schema = t["inputSchema"]
        assert t.get("description"), f"{name}: empty description"
        assert schema.get("type") == "object", f"{name}: schema not object"
        props = schema.get("properties", {})
        for req in schema.get("required", []):
            assert req in props, f"{name}: required '{req}' not in properties"
    for name, params in ENUMS.items():
        for param, values in params.items():
            enum = set(tools[name]["inputSchema"]["properties"][param].get("enum", []))
            assert enum == values, f"{name}.{param} enum {enum} != {values}"


def test_skill_parity(tools):
    skill = open(os.path.join(REPO, "claude/skills/palm-dev/SKILL.md")).read()
    documented = set(re.findall(r"`(palm_\w+)`", skill))
    served = set(tools)
    assert documented == served, (
        f"SKILL.md/proxy drift: only-in-docs={sorted(documented - served)} "
        f"only-in-proxy={sorted(served - documented)}")


def test_surface_complete(tools):
    assert sorted(tools) == sorted(EXPECTED), (
        f"surface mismatch: missing={sorted(set(EXPECTED) - set(tools))} "
        f"extra={sorted(set(tools) - set(EXPECTED))}")


if __name__ == "__main__":
    tools = get_tools()
    failures = 0
    for fn in (test_core_surface, test_skill_parity, test_surface_complete):
        try:
            fn(tools)
            print(f"PASS {fn.__name__}")
        except AssertionError as e:
            print(f"FAIL {fn.__name__}: {e}")
            failures += 1
    sys.exit(1 if failures else 0)
```

- [ ] **Step 2: Write the dispatch/translation test**

`tests/phase3/test_mcp_dispatch.py`:

```python
#!/usr/bin/env python3
"""Phase 3a translation tests: tool call -> exact ReControl TCP command,
central validation (no silent defaults), per-action multiline + idempotency.

Runs against a fake ReControl server — no emulator needed.
"""

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO)

from tests.lib.fake_recontrol import FakeReControl  # noqa: E402
from tests.lib.mcp_proxy import McpProxy  # noqa: E402

PASS = FAIL = 0


def check(label, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print(f"PASS {label}")
    else:
        FAIL += 1
        print(f"FAIL {label} {detail}")


def fresh():
    f = FakeReControl()
    f.script("state", "OK running")
    f.script("apps all", ["OK", " Memo Pad type=appl creator=memo"])
    f.script("apps", ["OK", " Memo Pad type=appl creator=memo"])
    f.script("break list", ["OK", " 0: empty"])
    f.script("log list", ["OK", " Serial 0"])
    f.script("check list", ["OK", " ScreenAccess off"])
    f.script("errorhandling get", ["OK", " ErrorOn show"])
    f.script("tap ", "OK delivered")
    return f, McpProxy(f.port)


def main():
    f, p = fresh()
    T = McpProxy.text
    E = McpProxy.is_error

    # --- happy-path translation ---
    r = p.call("palm_tap", {"x": 12, "y": 148})
    check("tap sends exact cmd", f.received[-1] == "tap 12 148", f.received[-1:])
    check("tap passthrough", T(r) == "OK delivered" and not E(r), T(r))

    p.call("palm_pen", {"action": "down", "x": 1, "y": 2})
    check("pen", f.received[-1] == "pen down 1 2", f.received[-1:])

    p.call("palm_menu", {"menu": "Options", "item": "About"})
    check("menu quoting", f.received[-1] == 'menu "Options" "About"', f.received[-1:])

    r = p.call("palm_apps")
    check("apps default", f.received[-1] == "apps", f.received[-1:])
    check("apps multiline", "Memo Pad" in T(r), T(r))
    p.call("palm_apps", {"all": True})
    check("apps all", f.received[-1] == "apps all", f.received[-1:])

    # --- central validation: nothing reaches the wire ---
    n = len(f.received)
    r = p.call("palm_tap", {"x": 12})
    check("missing arg is usage error", E(r) and "missing required argument 'y'" in T(r), T(r))
    r = p.call("palm_tap", {"x": "12", "y": 5})
    check("wrong type is usage error", E(r) and "must be an integer" in T(r), T(r))
    r = p.call("palm_tap", {"x": 1, "y": 2, "z": 3})
    check("unknown arg is usage error", E(r) and "unknown argument 'z'" in T(r), T(r))
    r = p.call("palm_pen", {"action": "sideways", "x": 1, "y": 2})
    check("enum violation is usage error", E(r) and "must be one of" in T(r), T(r))
    check("validation sent nothing", len(f.received) == n, f.received[n:])

    # --- debug groups ---
    r = p.call("palm_break", {"action": "list"})
    check("break list multiline", f.received[-1] == "break list" and "empty" in T(r), T(r))
    p.call("palm_break", {"action": "set", "idx": 0, "addr": "0x10C32A40"})
    check("break set", f.received[-1] == "break set 0 0x10C32A40", f.received[-1:])
    p.call("palm_break", {"action": "set", "idx": 1, "addr": "0x10000",
                          "condition": "d0 == 0"})
    check("break set cond", f.received[-1] == "break set 1 0x10000 d0 == 0", f.received[-1:])
    n = len(f.received)
    r = p.call("palm_break", {"action": "set", "idx": 0})
    check("break set needs addr", E(r) and "requires" in T(r) and len(f.received) == n, T(r))
    r = p.call("palm_break", {"action": "clear"})
    check("break clear needs idx", E(r) and "requires" in T(r), T(r))

    p.call("palm_watch", {"action": "set", "addr": "0x1000", "nbytes": 4})
    check("watch set", f.received[-1] == "watch set 0x1000 4", f.received[-1:])
    p.call("palm_spy", {"action": "set", "addr": "0x2000"})
    check("spy set", f.received[-1] == "spy set 0x2000", f.received[-1:])
    p.call("palm_log", {"action": "set", "category": "Serial", "level": 2})
    check("log set", f.received[-1] == "log set Serial 2", f.received[-1:])
    p.call("palm_gremlin", {"action": "new", "seed": 42, "events": 500})
    check("gremlin new", f.received[-1] == "gremlin new 42 500", f.received[-1:])
    p.call("palm_check", {"action": "set", "flag": "ScreenAccess", "on": True})
    check("check set on", f.received[-1] == "check set ScreenAccess on", f.received[-1:])
    p.call("palm_check", {"action": "set", "flag": "ScreenAccess", "on": False})
    check("check set off", f.received[-1] == "check set ScreenAccess off", f.received[-1:])
    p.call("palm_errorhandling", {"action": "set", "setting": "ErrorOn",
                                  "behavior": "continue"})
    check("errorhandling set", f.received[-1] == "errorhandling set ErrorOn continue",
          f.received[-1:])
    p.call("palm_profile", {"action": "init", "max": 30000, "depth": 20})
    check("profile init args", f.received[-1] == "profile init 30000 20", f.received[-1:])
    r = p.call("palm_profile", {"action": "dump"})
    check("profile dump needs path", E(r) and "requires" in T(r), T(r))

    p.call("palm_speed")
    check("speed query", f.received[-1] == "speed", f.received[-1:])
    p.call("palm_speed", {"value": "max"})
    check("speed max", f.received[-1] == "speed max", f.received[-1:])
    p.call("palm_speed", {"value": "150"})
    check("speed 150", f.received[-1] == "speed 150", f.received[-1:])
    r = p.call("palm_speed", {"value": "banana"})
    check("speed bad value", E(r) and "1-10000" in T(r), T(r))
    r = p.call("palm_speed", {"value": "20000"})
    check("speed out of range", E(r) and "1-10000" in T(r), T(r))

    p.call("palm_backtrace")
    check("backtrace", f.received[-1] == "backtrace", f.received[-1:])

    # --- ERR passthrough keeps isError ---
    f.script("gremlin status", "ERR busy: gremlin running")
    r = p.call("palm_gremlin", {"action": "status"})
    check("ERR passthrough", E(r) and "ERR busy" in T(r), T(r))

    p.close()
    f.close()

    # --- reconnect honesty: idempotent resends, non-idempotent reports ---
    f2 = FakeReControl()
    f2.script("state", ("close",))
    p2 = McpProxy(f2.port)
    r = p2.call("palm_ping")  # state -> close; idempotent -> reconnect+resend
    f2._rules.insert(0, ("state", "OK running"))
    # second attempt after reconnect hits the new rule
    check("idempotent resent", McpProxy.text(r) in ("pong",) or "ERR transient" in McpProxy.text(r))
    f2.close()
    p2.close()

    f3 = FakeReControl()
    f3.script("tap ", ("close",))
    p3 = McpProxy(f3.port)
    r = p3.call("palm_tap", {"x": 1, "y": 2})
    check("non-idempotent never resent",
          McpProxy.is_error(r) and "may or may not have executed" in McpProxy.text(r)
          and f3.received.count("tap 1 2") == 1, McpProxy.text(r))
    f3.close()
    p3.close()

    print(f"\n{PASS} passed, {FAIL} failed")
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Run both — expect RED on the old proxy**

```bash
touch tests/phase3/__init__.py
python3 tests/phase3/test_mcp_surface.py; python3 tests/phase3/test_mcp_dispatch.py
```

Expected: surface test FAILS (`palm_dbs` present, 10 new tools missing, no enums); dispatch test FAILS (old proxy sends `tap 12 0` for the missing-y case instead of a usage error, debug tools are "Unknown tool").

- [ ] **Step 4: Commit the red tests**

```bash
git add tests/phase3/
git commit -m "test(phase3a): surface drift gate + dispatch translation tests (red)"
```

---

### Task A4: proxy rebuild — single table, 27 core tools

**Files:**
- Modify: `src/pose64-mcp-proxy.cpp`
- Modify: `claude/skills/palm-dev/SKILL.md`, `docs/recontrol-protocol.md` (same commit)

- [ ] **Step 1: Delete the three drift-prone structures**

Remove from `src/pose64-mcp-proxy.cpp`:
- `cmd_is_idempotent()` (lines 49-62 incl. its comment block),
- `get_tools_list()` and the `make_schema`/`int_prop`/`str_prop`/`bool_prop` section header comment "Tool definitions for tools/list" (keep those four helper functions — they move under the new section),
- `dispatch_tool()`, `rc_tool()`, `rc_tool_multi()` (lines 537-723).

- [ ] **Step 2: Re-sign the transport functions**

`rc_command` and `rc_command_multi` take the idempotency decision as a parameter now (default false = never resend). Replace their signatures and the one use of `cmd_is_idempotent`:

```cpp
static std::string rc_command (const std::string& cmd, bool idempotent = false)
```

and inside, replace `if (cmd_is_idempotent (cmd))` with `if (idempotent)`. Same for:

```cpp
static std::string rc_command_multi (const std::string& cmd, bool idempotent = false)
```

whose RECV_LOST branch comment changes from "Multi-line commands (ui/info/apps) are read-only — safe to resend" to a flag check:

```cpp
    if (rc == RECV_LOST)
    {
        // Resend only when the caller marked this command idempotent.
        if (!idempotent || !tcp_reconnect () || !tcp_send (g_sock, full)
            || tcp_recv_line (g_sock, first_line) != RECV_OK)
            return "ERR transient: ReControl connection lost";
    }
```

Do NOT alter the `ERR timeout:` / `ERR transient:` message texts — `tests/phase1/repro_1_3_proxy.py` asserts them.

- [ ] **Step 3: Add the tool-table infrastructure**

Insert after the base64 section, replacing the deleted code:

```cpp
// ============================================================================
// Tool table — the single source of truth
// ============================================================================
//
// Every MCP tool is ONE entry in kTools[].  tools/list, dispatch, argument
// validation, and reconnect/idempotency policy all derive from it.  (Phase 3
// replaced three parallel structures that had drifted: the tools/list
// builder, the dispatch if-chain, and cmd_is_idempotent().)
//
// Multiline-ness and idempotency are per-ACTION, not per-tool ("break list"
// is multiline+idempotent, "break set" is neither), so each tool's builder
// returns them alongside the TCP command.

static json make_schema (json properties = {}, std::vector<std::string> required = {})
{
    json schema = {{"type", "object"}};
    if (!properties.empty ())
        schema["properties"] = properties;
    if (!required.empty ())
        schema["required"] = json (required);
    return schema;
}

static json int_prop (const std::string& desc)
{
    return {{"type", "integer"}, {"description", desc}};
}

static json str_prop (const std::string& desc)
{
    return {{"type", "string"}, {"description", desc}};
}

static json bool_prop (const std::string& desc)
{
    return {{"type", "boolean"}, {"description", desc}};
}

static json enum_prop (const std::string& desc, std::vector<std::string> values)
{
    json p = {{"type", "string"}, {"description", desc}};
    p["enum"] = json (values);
    return p;
}

struct BuiltCmd
{
    std::string cmd;        // TCP command to send (when err is empty)
    std::string err;        // non-empty => "ERR usage: <err>", nothing sent
    bool multiline  = false;
    bool idempotent = false;
};

static BuiltCmd built (std::string cmd, bool multiline = false, bool idempotent = false)
{
    BuiltCmd b;
    b.cmd = std::move (cmd);
    b.multiline = multiline;
    b.idempotent = idempotent;
    return b;
}

static BuiltCmd usage_err (std::string msg)
{
    BuiltCmd b;
    b.err = std::move (msg);
    return b;
}

using SchemaFn  = json (*) ();
using BuildFn   = BuiltCmd (*) (const json&);
using HandlerFn = json (*) (const json&, const json&);

struct ToolDef
{
    const char* name;
    const char* description;
    SchemaFn    schema;
    BuildFn     build;      // nullptr when custom is set
    HandlerFn   custom;     // nullptr for plain command tools
};

static std::string istr (const json& v)   // validated integer -> string
{
    return std::to_string (v.get<long long> ());
}

static std::string sstr (const json& v)   // validated string -> string
{
    return v.get<std::string> ();
}

// Generic argument validation against the tool's own schema: required
// arguments present, basic types correct, enum membership, no unknown
// arguments.  A failure returns the usage message and NOTHING is sent to
// the emulator — no silent defaults (the old proxy turned a missing 'x'
// into "tap 0 0").
static std::string validate_args (const json& schema, const json& args)
{
    if (schema.contains ("required"))
        for (const auto& r : schema["required"])
            if (!args.contains (r.get<std::string> ()))
                return "missing required argument '" + r.get<std::string> () + "'";

    const json props = schema.value ("properties", json::object ());
    for (auto it = args.begin (); it != args.end (); ++it)
    {
        if (!props.contains (it.key ()))
            return "unknown argument '" + it.key () + "'";
        const json& p = props[it.key ()];
        const std::string type = p.value ("type", "");
        if (type == "integer" && !it.value ().is_number_integer ())
            return "argument '" + it.key () + "' must be an integer";
        if (type == "string" && !it.value ().is_string ())
            return "argument '" + it.key () + "' must be a string";
        if (type == "boolean" && !it.value ().is_boolean ())
            return "argument '" + it.key () + "' must be a boolean";
        if (p.contains ("enum"))
        {
            bool ok = false;
            for (const auto& e : p["enum"])
                if (e == it.value ())
                    ok = true;
            if (!ok)
                return "argument '" + it.key () + "' must be one of " + p["enum"].dump ();
        }
    }
    return "";
}
```

- [ ] **Step 4: Add the custom handlers**

```cpp
// ============================================================================
// Custom handlers — tools whose MCP result is more than a command passthrough
// ============================================================================

static json h_ping (const json& id, const json&)
{
    std::string resp = rc_command ("state", true);
    if (resp.rfind ("OK", 0) == 0)
        return make_tool_result (id, "pong");
    return make_tool_result (id, resp, true);
}

static json h_state (const json& id, const json&)
{
    std::string state = rc_command ("state", true);
    std::string info  = rc_command_multi ("info", true);
    return make_tool_result (id, json ({{"state", state}, {"info", info}}).dump (2));
}

static json h_dialog (const json& id, const json& args)
{
    std::string respond = args.value ("respond", std::string ());
    if (respond.empty ())
        return make_tool_result (id, rc_command_multi ("dialog", true));
    std::string resp = rc_command ("dialog respond " + respond);
    return make_tool_result (id, resp, resp.rfind ("OK", 0) != 0);
}

static json h_screenshot (const json& id, const json& args)
{
    std::string path = args.value ("path", std::string ());
    bool has_path = !path.empty ();
    if (!has_path)
        path = "/tmp/pose64_screenshot.png";

    std::string cmd = "screenshot " + path;

    int  scale    = args.value ("scale", 1);
    bool grid     = args.value ("grid", false);
    bool annotate = args.value ("annotate", false);
    std::string crosshair = args.value ("crosshair", std::string ());

    crosshair.erase (std::remove (crosshair.begin (), crosshair.end (), ' '), crosshair.end ());
    crosshair.erase (std::remove (crosshair.begin (), crosshair.end (), '\n'), crosshair.end ());
    if (!crosshair.empty () && crosshair.find (',') == std::string::npos)
        return make_tool_result (id, "ERR usage: crosshair must be 'x,y' (e.g. '80,72')", true);

    if (scale > 1)
        cmd += " scale=" + std::to_string (scale);
    if (grid)
        cmd += " grid";
    if (annotate)
        cmd += " annotate";
    if (!crosshair.empty ())
        cmd += " crosshair=" + crosshair;

    std::string resp = rc_command (cmd);
    if (resp.substr (0, 2) != "OK")
        return make_tool_result (id, resp, true);

    if (has_path)
    {
        if (annotate)
        {
            std::string ui_text = rc_command_multi ("ui", true);
            return make_tool_result (id, resp + "\n" + ui_text);
        }
        return make_tool_result (id, resp);
    }

    std::ifstream file (path, std::ios::binary);
    if (!file)
        return make_tool_result (id, "ERR: could not read " + path, true);

    std::vector<uint8_t> data ((std::istreambuf_iterator<char> (file)),
                                std::istreambuf_iterator<char> ());
    std::string b64 = base64_encode (data);

    if (annotate)
    {
        std::string ui_text = rc_command_multi ("ui", true);
        return make_tool_image_text (id, b64, ui_text);
    }

    return make_tool_image (id, b64);
}
```

- [ ] **Step 5: Add the table — 27 core entries**

```cpp
// ============================================================================
// The catalog.  27 core tools (this task) + 10 debug-surface tools (next).
// ============================================================================

static const ToolDef kTools[] = {

{ "palm_ping", "Ping the emulator to check if it is responsive.",
  [] { return make_schema (); },
  nullptr, h_ping },

{ "palm_state", "Get emulator state (running/suspended/blocked_on_ui) and device info as JSON.",
  [] { return make_schema (); },
  nullptr, h_state },

{ "palm_ui", "Read the current Palm OS form/UI structure: object types, IDs, labels, bounds, text.",
  [] { return make_schema (); },
  [] (const json&) { return built ("ui", true, true); }, nullptr },

{ "palm_apps", "List installed applications. Set all=true to list EVERY database "
  "(data, resources, libraries), not just launchable apps.",
  [] { return make_schema ({{"all", bool_prop ("List all databases, not just apps (default false)")}}); },
  [] (const json& a) { return built (a.value ("all", false) ? "apps all" : "apps", true, true); },
  nullptr },

{ "palm_tap", "Tap at screen coordinates (0-159). Returns 'OK delivered' only once the "
  "guest's event queue actually has the event (delivery-honest, blocks <=2s); a refused "
  "or undelivered tap is a truthful ERR, never a silent OK.",
  [] { return make_schema ({{"x", int_prop ("X coordinate (0-159)")},
                            {"y", int_prop ("Y coordinate (0-159)")}}, {"x", "y"}); },
  [] (const json& a) { return built ("tap " + istr (a["x"]) + " " + istr (a["y"])); }, nullptr },

{ "palm_tap_id", "Tap the center of a form object by its numeric ID (from palm_ui). "
  "Delivery-honest like palm_tap.",
  [] { return make_schema ({{"id", int_prop ("Object ID from palm_ui")}}, {"id"}); },
  [] (const json& a) { return built ("tap-id " + istr (a["id"])); }, nullptr },

{ "palm_pen", "Send a single pen down or up event at coordinates. Delivery-honest.",
  [] { return make_schema ({{"action", enum_prop ("Pen action", {"down", "up"})},
                            {"x", int_prop ("X coordinate (0-159)")},
                            {"y", int_prop ("Y coordinate (0-159)")}},
                           {"action", "x", "y"}); },
  [] (const json& a) { return built ("pen " + sstr (a["action"]) + " "
                                     + istr (a["x"]) + " " + istr (a["y"])); }, nullptr },

{ "palm_key", "Send a key event by decimal character code. Delivery-honest.",
  [] { return make_schema ({{"code", int_prop ("Character code (decimal)")}}, {"code"}); },
  [] (const json& a) { return built ("key " + istr (a["code"])); }, nullptr },

{ "palm_type", "Type a string of text (UTF-8 in, converted to Latin-1). Delivery-honest.",
  [] { return make_schema ({{"text", str_prop ("Text to type")}}, {"text"}); },
  [] (const json& a) { return built ("type " + sstr (a["text"])); }, nullptr },

{ "palm_button", "Press a hardware button. Queued contract: OK means enqueued to the "
  "hardware-button state, not delivery-confirmed; ERR busy when gremlin/playback active.",
  [] { return make_schema ({{"name", enum_prop ("Button", {"power", "up", "down", "app1",
                                                           "app2", "app3", "app4",
                                                           "cradle", "contrast"})},
                            {"action", enum_prop ("Action", {"down", "up", "tap"})}},
                           {"name", "action"}); },
  [] (const json& a) { return built ("button " + sstr (a["name"]) + " " + sstr (a["action"])); },
  nullptr },

{ "palm_screenshot", "Take a screenshot. Returns base64 image data if no path given, else "
  "saves PNG to path. scale/grid/annotate/crosshair add AI-friendly coordinate overlays; "
  "the returned CRC is always of raw pre-overlay pixels.",
  [] { return make_schema ({{"path", str_prop ("File path to save PNG (optional; default returns image data)")},
                            {"scale", int_prop ("Integer upscale, e.g. 4 for 640x640 (default 1, max 16)")},
                            {"grid", bool_prop ("Coordinate grid overlay with rulers (default false)")},
                            {"annotate", bool_prop ("UI bounding boxes with IDs; also returns palm_ui text (default false)")},
                            {"crosshair", str_prop ("Mark a point: 'x,y', e.g. '80,72'")}}); },
  nullptr, h_screenshot },

{ "palm_screen_hash", "CRC32 hash of current screen pixels + dimensions (fast change detection).",
  [] { return make_schema (); },
  [] (const json&) { return built ("screen-hash", false, true); }, nullptr },

{ "palm_launch", "Launch an application by database name (names with spaces are fine).",
  [] { return make_schema ({{"app", str_prop ("Application database name (from palm_apps)")}}, {"app"}); },
  [] (const json& a) { return built ("launch " + sstr (a["app"])); }, nullptr },

{ "palm_install", "Install a .prc/.pdb file into the emulator (max 4MB).",
  [] { return make_schema ({{"path", str_prop ("Path to .prc or .pdb file")}}, {"path"}); },
  [] (const json& a) { return built ("install " + sstr (a["path"])); }, nullptr },

{ "palm_export", "Export a database from the emulator to a host .prc/.pdb file.",
  [] { return make_schema ({{"db", str_prop ("Database name (from palm_apps)")},
                            {"path", str_prop ("Host file path to write")}}, {"db", "path"}); },
  [] (const json& a) { return built ("export " + sstr (a["db"]) + " " + sstr (a["path"])); },
  nullptr },

{ "palm_save", "Save the current emulator session to a .psf file.",
  [] { return make_schema ({{"path", str_prop ("Path to save session file")}}, {"path"}); },
  [] (const json& a) { return built ("save " + sstr (a["path"])); }, nullptr },

{ "palm_load", "Load an emulator session from a .psf file (replaces the current session).",
  [] { return make_schema ({{"path", str_prop ("Path to session file")}}, {"path"}); },
  [] (const json& a) { return built ("load " + sstr (a["path"])); }, nullptr },

{ "palm_reset", "Reset the emulated device. Works even in blocked_on_ui (dismisses any dialog).",
  [] { return make_schema ({{"type", enum_prop ("Reset type (default soft)", {"soft", "hard", "debug"})}}); },
  [] (const json& a) { return built (a.contains ("type") ? "reset " + sstr (a["type"]) : "reset"); },
  nullptr },

{ "palm_sleep", "Pause command processing for 1-30000 milliseconds.",
  [] { return make_schema ({{"ms", int_prop ("Milliseconds (1-30000)")}}, {"ms"}); },
  [] (const json& a) { return built ("sleep " + istr (a["ms"])); }, nullptr },

{ "palm_quit", "Quit the emulator process.",
  [] { return make_schema (); },
  [] (const json&) { return built ("quit"); }, nullptr },

{ "palm_dialog", "Query a pending modal dialog (message, buttons, full CPU register dump when "
  "blocked_on_ui), or respond to dismiss it. Omit 'respond' to just query.",
  [] { return make_schema ({{"respond", enum_prop ("Button to click (omit to query)",
                                                   {"ok", "cancel", "continue", "debug",
                                                    "reset", "yes", "no"})}}); },
  nullptr, h_dialog },

{ "palm_run", "Execute a batch of commands in one call, separated by semicolons. "
  "Sub-commands: tap, pen, key, type, button, sleep, repeat N { ... }. Queued contract "
  "(not delivery-confirmed). Example: 'tap 12 148; sleep 150; type x'.",
  [] { return make_schema ({{"script", str_prop ("Semicolon-separated commands")}}, {"script"}); },
  [] (const json& a) { return built ("run " + sstr (a["script"])); }, nullptr },

{ "palm_peek", "Read 1-256 bytes from emulated memory. Address formats: 0x<hex> (absolute), "
  "a5@<offset> (A5-relative), global.<name> (low-memory global). Works in blocked_on_ui.",
  [] { return make_schema ({{"addr", str_prop ("Address (0x<hex>, a5@<offset>, global.<name>)")},
                            {"nbytes", int_prop ("Bytes to read (1-256)")}}, {"addr", "nbytes"}); },
  [] (const json& a) { return built ("peek " + sstr (a["addr"]) + " " + istr (a["nbytes"]),
                                     false, true); }, nullptr },

{ "palm_poke", "Write bytes to emulated memory.",
  [] { return make_schema ({{"addr", str_prop ("Address (0x<hex>, a5@<offset>, global.<name>)")},
                            {"nbytes", int_prop ("Bytes to write (1-256)")},
                            {"data", str_prop ("Hex string of bytes (e.g. '00A1B2C3')")}},
                           {"addr", "nbytes", "data"}); },
  [] (const json& a) { return built ("poke " + sstr (a["addr"]) + " " + istr (a["nbytes"])
                                     + " " + sstr (a["data"])); }, nullptr },

{ "palm_regs", "Read all m68k CPU registers (D0-D7, A0-A7, PC, SR). Works in blocked_on_ui.",
  [] { return make_schema (); },
  [] (const json&) { return built ("regs", false, true); }, nullptr },

{ "palm_menu", "Trigger a menu item by menu title and item title (posts a menuEvent).",
  [] { return make_schema ({{"menu", str_prop ("Menu title (e.g. 'Options')")},
                            {"item", str_prop ("Item title or substring (e.g. 'About')")}},
                           {"menu", "item"}); },
  [] (const json& a) { return built ("menu \"" + sstr (a["menu"]) + "\" \""
                                     + sstr (a["item"]) + "\""); }, nullptr },

{ "palm_delete", "Delete a database from the emulated device.",
  [] { return make_schema ({{"db", str_prop ("Database name to delete")}}, {"db"}); },
  [] (const json& a) { return built ("delete " + sstr (a["db"])); }, nullptr },

};  // kTools

static const ToolDef* find_tool (const std::string& name)
{
    for (const ToolDef& t : kTools)
        if (name == t.name)
            return &t;
    return nullptr;
}
```

- [ ] **Step 6: Add the generic dispatcher and table-driven tools/list**

```cpp
// ============================================================================
// Dispatch — one path for every tool
// ============================================================================

static json dispatch_tool (const json& id, const std::string& name, const json& args)
{
    const ToolDef* t = find_tool (name);
    if (!t)
        return make_tool_result (id, "Unknown tool: " + name, true);

    std::string verr = validate_args (t->schema (), args);
    if (!verr.empty ())
        return make_tool_result (id, "ERR usage: " + verr, true);

    if (t->custom)
        return t->custom (id, args);

    BuiltCmd b = t->build (args);
    if (!b.err.empty ())
        return make_tool_result (id, "ERR usage: " + b.err, true);

    std::string resp = b.multiline ? rc_command_multi (b.cmd, b.idempotent)
                                   : rc_command (b.cmd, b.idempotent);
    return make_tool_result (id, resp, resp.rfind ("OK", 0) != 0);
}
```

and replace `handle_tools_list`'s body:

```cpp
static json handle_tools_list (const json& id)
{
    json tools = json::array ();
    for (const ToolDef& t : kTools)
        tools.push_back ({{"name", t.name},
                          {"description", t.description},
                          {"inputSchema", t.schema ()}});
    return make_result (id, {{"tools", tools}});
}
```

(`handle_tools_call` and `main()` are unchanged.)

- [ ] **Step 7: Build and run the tests**

```bash
cmake --build build --target pose64-mcp-proxy -j$(nproc)
python3 tests/phase3/test_mcp_surface.py
python3 tests/phase3/test_mcp_dispatch.py
python3 tests/phase1/repro_1_3_proxy.py
```

Expected: surface test — `test_core_surface` PASS, `test_skill_parity`/`test_surface_complete` still FAIL (10 new tools missing; SKILL.md updated this commit covers only core). Dispatch test — all CORE cases pass; debug-group cases FAIL ("Unknown tool"). repro_1_3 PASS (timeout/resend behavior preserved).

- [ ] **Step 8: Update SKILL.md and protocol doc (same commit)**

In `claude/skills/palm-dev/SKILL.md`: delete the `palm_dbs` table row; change the `palm_apps` row to `| palm_apps | all (optional) | Installed applications; all=true lists every database |`; update the tool-count sentence if present. In `docs/recontrol-protocol.md`: in the "Not MCP tools" banner of the Debugging section, change "exactly 28 `palm_*` tools" to "27 `palm_*` tools (the 10 debug-surface tools land in the next commit)" — transitional but truthful.

- [ ] **Step 9: Commit**

```bash
git add src/pose64-mcp-proxy.cpp claude/skills/palm-dev/SKILL.md docs/recontrol-protocol.md
git commit -m "feat(phase3a): single-table proxy core — central validation, per-action idempotency, palm_dbs absorbed (R2)"
```

---

### Task A5: the 10 new debug-surface tools

**Files:**
- Modify: `src/pose64-mcp-proxy.cpp` (append entries to `kTools[]` before the closing `};`)
- Modify: `claude/skills/palm-dev/SKILL.md`, `docs/recontrol-protocol.md` (same commit)

- [ ] **Step 1: Append the 10 entries**

```cpp
{ "palm_speed", "Set or query emulation speed. '100' = 1x wall-clock, '200' = 2x, 'max' = "
  "unthrottled (pegs a host core by design — use briefly). Omit value to query.",
  [] { return make_schema ({{"value", str_prop ("Percent 1-10000, or 'max'. Omit to query.")}}); },
  [] (const json& a) -> BuiltCmd {
      if (!a.contains ("value"))
          return built ("speed", false, true);
      std::string v = sstr (a["value"]);
      if (v == "max")
          return built ("speed max");
      char* end = nullptr;
      long pct = strtol (v.c_str (), &end, 10);
      if (end == v.c_str () || *end != '\0' || pct < 1 || pct > 10000)
          return usage_err ("value must be 1-10000 or 'max'");
      return built ("speed " + std::to_string (pct));
  }, nullptr },

{ "palm_backtrace", "Stack crawl of the emulated m68k CPU: PC and A6 per frame. Works in "
  "blocked_on_ui (frozen CPU) — the first tool to reach for after a crash dialog.",
  [] { return make_schema (); },
  [] (const json&) { return built ("backtrace", true, true); }, nullptr },

{ "palm_break", "Manage the 6 m68k breakpoint slots. action=set requires idx+addr (condition "
  "optional, e.g. 'd0 == 0'); clear/enable/disable require idx. "
  "WARNING (landmine #1): a hit currently stops execution ONLY when an external SLP "
  "debugger is attached; with none the hit is silently ignored. Use palm_watch/palm_spy "
  "to stop on memory writes. (Phase 3b makes hits raise a real dialog.)",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "clear",
                                                                "enable", "disable", "clearall"})},
                            {"idx", int_prop ("Breakpoint slot 0-5")},
                            {"addr", str_prop ("Code address, e.g. '0x10C32A40' (set)")},
                            {"condition", str_prop ("Optional condition (set), e.g. 'd0 == 0'")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("break list", true, true);
      if (act == "clearall")
          return built ("break clearall");
      if (act == "set")
      {
          if (!a.contains ("idx") || !a.contains ("addr"))
              return usage_err ("action 'set' requires 'idx' and 'addr'");
          std::string cmd = "break set " + istr (a["idx"]) + " " + sstr (a["addr"]);
          if (a.contains ("condition"))
              cmd += " " + sstr (a["condition"]);
          return built (cmd);
      }
      if (!a.contains ("idx"))
          return usage_err ("action '" + act + "' requires 'idx'");
      return built ("break " + act + " " + istr (a["idx"]));
  }, nullptr },

{ "palm_watch", "Watchpoint: stop when the guest WRITES the address range. On hit the CPU "
  "blocks on a Continue/Debug/Reset dialog (state=blocked_on_ui): inspect with "
  "palm_dialog/palm_backtrace, resume with palm_dialog respond=continue. One at a time.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"set", "clear", "status"})},
                            {"addr", str_prop ("Start address (set)")},
                            {"nbytes", int_prop ("Range length 1-65536 (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("watch status", false, true);
      if (act == "clear")
          return built ("watch clear");
      if (!a.contains ("addr") || !a.contains ("nbytes"))
          return usage_err ("action 'set' requires 'addr' and 'nbytes'");
      return built ("watch set " + sstr (a["addr"]) + " " + istr (a["nbytes"]));
  }, nullptr },

{ "palm_spy", "Step spy: stop when the VALUE at a single address changes. Raises the same "
  "Continue/Debug/Reset dialog as palm_watch on hit. One at a time.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"set", "clear", "status"})},
                            {"addr", str_prop ("Address to monitor (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("spy status", false, true);
      if (act == "clear")
          return built ("spy clear");
      if (!a.contains ("addr"))
          return usage_err ("action 'set' requires 'addr'");
      return built ("spy set " + sstr (a["addr"]));
  }, nullptr },

{ "palm_log", "Emulator event logging: 20 categories (action=list shows them), levels "
  "0=off 1=during-gremlins 2=always. dump flushes the buffer to file, clear empties it.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "dump", "clear"})},
                            {"category", str_prop ("Category name from action=list (set)")},
                            {"level", int_prop ("0=off, 1=gremlin, 2=always (set)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("log list", true, true);
      if (act == "set")
      {
          if (!a.contains ("category") || !a.contains ("level"))
              return usage_err ("action 'set' requires 'category' and 'level'");
          return built ("log set " + sstr (a["category"]) + " " + istr (a["level"]));
      }
      return built ("log " + act);
  }, nullptr },

{ "palm_gremlin", "Automated random-event stress testing (Hordes). action=new requires "
  "seed+events. WARNING: while a gremlin runs, normal input tools return "
  "'ERR busy: gremlin running' — stop the gremlin first.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"new", "status", "suspend",
                                                                "step", "resume", "stop"})},
                            {"seed", int_prop ("Random seed (new)")},
                            {"events", int_prop ("Number of events to post (new)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "status")
          return built ("gremlin status", false, true);
      if (act == "new")
      {
          if (!a.contains ("seed") || !a.contains ("events"))
              return usage_err ("action 'new' requires 'seed' and 'events'");
          return built ("gremlin new " + istr (a["seed"]) + " " + istr (a["events"]));
      }
      return built ("gremlin " + act);
  }, nullptr },

{ "palm_check", "MetaMemory access-check flags (18; action=list shows them). "
  "WARNING (landmine #7): enabling any DRAM-region flag (LowMemoryAccess, "
  "SystemGlobalAccess, ScreenAccess, MemMgrDataAccess, FreeChunkAccess, "
  "UnlockedChunkAccess) re-arms an O(n) heap scan per memory access — 100% CPU within "
  "~10 minutes. Enable briefly for a targeted test, then action=clearall.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"list", "set", "set-all", "clearall"})},
                            {"flag", str_prop ("Flag name from action=list (set)")},
                            {"on", bool_prop ("true=on, false=off (set/set-all)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "list")
          return built ("check list", true, true);
      if (act == "clearall")
          return built ("check clearall");
      if (!a.contains ("on"))
          return usage_err ("action '" + act + "' requires 'on'");
      std::string onoff = a["on"].get<bool> () ? "on" : "off";
      if (act == "set-all")
          return built ("check set-all " + onoff);
      if (!a.contains ("flag"))
          return usage_err ("action 'set' requires 'flag' and 'on'");
      return built ("check set " + sstr (a["flag"]) + " " + onoff);
  }, nullptr },

{ "palm_errorhandling", "Query or set how the emulator responds to guest errors/warnings "
  "(per-setting behavior: show dialog, auto-continue, quit, or switch).",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"get", "set"})},
                            {"setting", enum_prop ("Which setting (set)",
                                                   {"WarningOff", "ErrorOff", "WarningOn", "ErrorOn"})},
                            {"behavior", enum_prop ("Behavior (set)",
                                                    {"show", "continue", "quit", "switch"})}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      if (sstr (a["action"]) == "get")
          return built ("errorhandling get", true, true);
      if (!a.contains ("setting") || !a.contains ("behavior"))
          return usage_err ("action 'set' requires 'setting' and 'behavior'");
      return built ("errorhandling set " + sstr (a["setting"]) + " " + sstr (a["behavior"]));
  }, nullptr },

{ "palm_profile", "Metrowerks-format CPU profiler. Order matters: init -> start -> stop -> "
  "dump (requires path; writes .mwp plus a .txt sibling). cycles queries raw counters. "
  "cleanup frees profiler memory.",
  [] { return make_schema ({{"action", enum_prop ("Operation", {"init", "start", "stop", "dump",
                                                                "print", "cleanup", "cycles"})},
                            {"max", int_prop ("Max functions (init, optional; requires depth)")},
                            {"depth", int_prop ("Max stack depth (init, optional)")},
                            {"path", str_prop ("Output file path (dump/print)")}},
                           {"action"}); },
  [] (const json& a) -> BuiltCmd {
      std::string act = sstr (a["action"]);
      if (act == "cycles")
          return built ("profile cycles", false, true);
      if (act == "init")
      {
          std::string cmd = "profile init";
          if (a.contains ("max") && a.contains ("depth"))
              cmd += " " + istr (a["max"]) + " " + istr (a["depth"]);
          else if (a.contains ("max") || a.contains ("depth"))
              return usage_err ("action 'init' takes 'max' and 'depth' together");
          return built (cmd);
      }
      if (act == "dump" || act == "print")
      {
          if (!a.contains ("path"))
              return usage_err ("action '" + act + "' requires 'path'");
          return built ("profile " + act + " " + sstr (a["path"]));
      }
      return built ("profile " + act);
  }, nullptr },
```

- [ ] **Step 2: Build and run all phase-3 tests**

```bash
cmake --build build --target pose64-mcp-proxy -j$(nproc)
python3 tests/phase3/test_mcp_dispatch.py
python3 tests/phase3/test_mcp_surface.py
```

Expected: dispatch test ALL PASS. Surface test: `test_core_surface` + `test_surface_complete` PASS; `test_skill_parity` FAILS until Step 3 lands.

- [ ] **Step 3: Update SKILL.md + protocol doc (same commit)**

SKILL.md: add 10 rows to the MCP tool table:

```markdown
| `palm_speed` | `value` (optional) | Set/query emulation speed: 1-10000 percent or `max`; omit to query |
| `palm_backtrace` | -- | m68k stack crawl (works in `blocked_on_ui`) |
| `palm_break` | `action`, `idx`, `addr`, `condition` | Manage 6 breakpoint slots (list/set/clear/enable/disable/clearall) |
| `palm_watch` | `action`, `addr`, `nbytes` | Watchpoint: dialog-stop when range is written (set/clear/status) |
| `palm_spy` | `action`, `addr` | Step spy: dialog-stop when value changes (set/clear/status) |
| `palm_log` | `action`, `category`, `level` | Event logging: 20 categories, levels 0/1/2 (list/set/dump/clear) |
| `palm_gremlin` | `action`, `seed`, `events` | Hordes stress testing (new/status/suspend/step/resume/stop) |
| `palm_check` | `action`, `flag`, `on` | MetaMemory access checks — see landmine #7 warning in description |
| `palm_errorhandling` | `action`, `setting`, `behavior` | Guest error/warning behavior (get/set) |
| `palm_profile` | `action`, `max`, `depth`, `path` | CPU profiler: init→start→stop→dump |
```

Also add a "## Debugging workflows" section after "Efficiency Patterns" describing: crash triage (`palm_state` → `blocked_on_ui` → `palm_dialog` → `palm_backtrace`/`palm_peek` → `palm_dialog respond=…`), watchpoint stop-on-write, and the truthful break caveat (passive until Phase 3b — copy the warning wording from the tool description).

`docs/recontrol-protocol.md`: replace the Debugging section's "**Not MCP tools.**" banner paragraph with: "All commands in this section (and Logging, Gremlins, Configuration, Profiling below) are exposed as MCP tools (`palm_break`, `palm_watch`, `palm_spy`, `palm_log`, `palm_gremlin`, `palm_check`, `palm_errorhandling`, `palm_profile`, `palm_backtrace`, `palm_speed`) — raw TCP is no longer needed for any of them." Keep the `break`-is-passive warning paragraph (still true until Plan 3b).

- [ ] **Step 4: Re-run surface test — all green**

```bash
python3 tests/phase3/test_mcp_surface.py
```

Expected: all three test functions PASS.

- [ ] **Step 5: Live smoke against a real emulator**

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, ".")
from tests.lib.harness import emulator
from tests.lib.mcp_proxy import McpProxy
with emulator(6427):
    p = McpProxy(6427)
    for name, args in [("palm_state", {}), ("palm_break", {"action": "list"}),
                       ("palm_log", {"action": "list"}), ("palm_speed", {}),
                       ("palm_backtrace", {}), ("palm_check", {"action": "list"})]:
        r = p.call(name, args)
        assert not McpProxy.is_error(r), f"{name}: {McpProxy.text(r)}"
        print(name, "OK")
    p.close()
print("SMOKE PASS")
EOF
```

Expected: six OK lines + `SMOKE PASS`.

- [ ] **Step 6: Commit**

```bash
git add src/pose64-mcp-proxy.cpp claude/skills/palm-dev/SKILL.md docs/recontrol-protocol.md
git commit -m "feat(phase3a): expose debug surface as MCP tools — 37-tool catalog complete (task 3.1)"
```

---

### Task A6: consolidate the remaining Python clients (task 3.4)

**Files:**
- Create: `tests/test_cpu_worker_tap.py`
- Delete: `test_cpu_worker_tap.py`, `datebook_interaction.py`, `garak_intrigue.py`

- [ ] **Step 1: Port test_cpu_worker_tap.py onto the shared client/harness**

Create `tests/test_cpu_worker_tap.py`:

```python
#!/usr/bin/env python3
"""Integration test: input commands execute while the CPU worker runs.

Ported from the root-level scratch script onto tests/lib (Phase 3a, task 3.4).
"""

import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from tests.lib.harness import emulator, connect  # noqa: E402


def main():
    port = 6427
    with emulator(port):
        c = connect(port)
        try:
            for cmd in ("state", "tap 50 40", "key 65", "state"):
                resp = c.send_command(cmd)
                print(f"{cmd!r} -> {resp.strip() if resp else resp!r}")
                assert resp and resp.startswith("OK"), f"{cmd} failed: {resp}"
        finally:
            c.disconnect()
    print("PASS")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Delete the scratch scripts**

```bash
git rm test_cpu_worker_tap.py datebook_interaction.py garak_intrigue.py
```

- [ ] **Step 3: Run the ported test**

```bash
python3 tests/test_cpu_worker_tap.py
```

Expected: four `OK` lines + `PASS`.

- [ ] **Step 4: Commit**

```bash
git add tests/test_cpu_worker_tap.py
git commit -m "refactor(phase3a): consolidate Python clients onto tests/lib; delete scratch scripts (task 3.4)"
```

---

### Task A7: Plan 3a close-out — docs and banner

**Files:**
- Modify: `docs/STATUS.md`, `claude/agents/pose64-tester.md`, `docs/recovery-plan-2026-06.md`

- [ ] **Step 1: STATUS.md**

In "What verifiably works", replace the MCP proxy bullet with:

```markdown
- **MCP proxy**: **37** `palm_*` tools served from a single source-of-truth
  table (tools/list, dispatch, and reconnect/idempotency policy all derive
  from it — Phase 3a). Central argument validation: a missing/invalid
  argument is `ERR usage`, never a silent default. The full debug surface
  (backtrace/break/watch/spy/log/gremlin/check/errorhandling/profile/speed)
  is MCP-exposed; drift between proxy and SKILL.md is test-gated
  (`tests/phase3/test_mcp_surface.py`).
```

- [ ] **Step 2: pose64-tester.md**

Update its tool list/capability text to match the 37-tool surface (add the 10 new tools with one-line descriptions mirroring SKILL.md; remove `palm_dbs`, note `palm_apps all=true`).

- [ ] **Step 3: Recovery-plan banner**

Update the CURRENT POSITION banner (top of `docs/recovery-plan-2026-06.md`): Phase 3 is IN PROGRESS under the expanded scope of `docs/superpowers/specs/2026-06-11-phase3-mcp-debug-layer-design.md`, executed as three plans (3a MCP surface — DONE with this commit; 3b debugger fixes — NEXT; 3c MetaMemory + GATE 3). In the Phase 3 section body, annotate tasks 3.1 and 3.4 as done via plan 3a, and note that 3.2/3.3 are plan 3b and the check/landmine-7 fix + GATE 3 are plan 3c.

- [ ] **Step 4: Full verification sweep, then commit**

```bash
python3 tests/phase3/test_mcp_surface.py && python3 tests/phase3/test_mcp_dispatch.py
for r in tests/phase1/repro_*.py; do python3 "$r" || echo "FAILED: $r"; done
git add docs/STATUS.md claude/agents/pose64-tester.md docs/recovery-plan-2026-06.md
git commit -m "docs(phase3a): STATUS + tester + banner — 37-tool surface landed; next: plan 3b"
```

Expected: phase-3 tests green; phase-1 repros 7/7 PASS (the proxy repro exercises the rebuilt binary).

---

## Self-review notes (kept for the executor)

- The `ERR timeout:`/`ERR transient:` strings in `rc_command`/`rc_command_multi` are load-bearing (repro_1_3 asserts them) — do not rewrite them.
- `tests/phase3/test_mcp_dispatch.py`'s reconnect cases poke `FakeReControl._rules` directly; that is intentional test-only access.
- All file writes must be LF-only (project rule); run `file <path>` after creating each.
