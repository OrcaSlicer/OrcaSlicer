#!/usr/bin/env python3
"""Checks that a background job an MCP call starts holds the project until the UI job worker is idle.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_worker_admission.py

Fills the bed with small cubes, so an arrange with rotation runs for many seconds, and starts it three
ways: the typed `arrange`, `run_action obj_arrange`, and a click on the Arrange options panel's
Arrange button, a recover input. While each runs, a `transform`, `ui_key Delete` on the canvas and a
GL toolbar click are refused and `list_objects` answers; `cancel` stops it, the call reports it
cancelled, and a change is admitted once the worker is idle. Exits 0 when every check passes.
"""

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

# The typed arrange goes first: the rotation it saves, as the panel's checkbox does, makes the
# others slow too.
STARTERS = {
    "arrange": ("arrange", {"rotate": True}),
    "run_action": ("run_action", {"id": "orca_command:obj_arrange:orca"}),
    "ui_click": ("ui_click", {"target": {"imgui": "Arrange", "window": "Arrange options"}}),
}


def client():
    c, _ = orca_mcp.session_client()
    c.initialize()
    return c


def attempt(c, tool, **args):
    try:
        return True, c.call(tool, **args)
    except orca_mcp.McpError as e:
        return False, str(e)


def main():
    c = client()
    failures = []

    def check(name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:300]))
        if not ok:
            failures.append(name)

    c.call("wait", **{"for": "ready", "timeout_s": 50})
    c.call("new_project", discard_changes=True)
    c.call("set_view", tab="prepare")
    for size in ["6 6 6", "4 9 5", "7 3 5"]:
        c.call("select", none=True)
        c.call("run_action", id="orca_command:add_primitive_cube:orca", param=size)
    for o in range(3):
        c.call("arrange", fill=o, spacing=0, rotate=False)

    for name, (tool, args) in STARTERS.items():
        if name == "ui_click" and c.call("ui_snapshot", query="Arrange options", include_menus=False).get("nodes", 0) == 0:
            c.call("ui_click", target={"gltoolbar": "arrange"})
        out = {}

        def run():
            out["reply"] = attempt(client(), tool, **args)

        starter = threading.Thread(target=run)
        starter.start()
        time.sleep(2.0)
        ok, reply = attempt(c, "transform", object=0, translate=[1, 0, 0])
        check(name + ": transform refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "list_objects", limit=1)
        check(name + ": list_objects answers", ok, reply)
        ok, reply = attempt(c, "ui_key", keys="Delete")
        check(name + ": ui_key Delete refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "ui_click", target={"gltoolbar": "orient"})
        check(name + ": GL toolbar click refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "cancel")
        check(name + ": cancel stops it", ok and reply.get("was_running") and reply.get("job"), reply)
        starter.join(300)
        ok, reply = out.get("reply", (True, None))
        check(name + ": the call reports it cancelled", not ok and "cancelled" in reply, reply)
        ok, reply = attempt(c, "transform", object=0, translate=[-1, 0, 0])
        check(name + ": a change is admitted afterwards", ok, reply)

    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
