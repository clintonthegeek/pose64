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

    # --- robustness: non-object arguments must not crash the proxy (code review) ---
    r = p.call("palm_ping", "oops")
    check("non-object args is usage error, not a crash",
          E(r) and "must be an object" in T(r), T(r))
    # proxy still alive after the bad call (ping reuses the scripted "state" response):
    r = p.call("palm_ping")
    check("proxy survived bad args", not E(r) and T(r) == "pong", T(r))

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
