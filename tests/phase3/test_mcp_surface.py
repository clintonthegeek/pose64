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
