#!/usr/bin/env python3
"""Checks that a background job an MCP call starts holds the project until the UI job worker is idle.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_worker_admission.py

Fills the bed with small cubes, so an arrange with rotation runs for many seconds, and starts it three
ways: `object_arrange` with arguments, `action_run obj_arrange`, and a click on the Arrange options panel's
Arrange button, a recover input. While each runs, an `object_transform`, `keyboard_press Delete` on the canvas and a
GL toolbar click are refused and `object_list` answers; `job_cancel` stops it, the call reports it
cancelled, and a change is admitted once the worker is idle. Exits 0 when every check passes.
"""

import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import checks  # noqa: E402
import orca_mcp  # noqa: E402

# object_arrange goes first: the rotation it saves, as the panel's checkbox does, makes the
# others slow too.
STARTERS = {
    "object_arrange": ("object_arrange", {"rotate": True}),
    "action_run": ("action_run", {"id": "orca_command:obj_arrange:orca"}),
    "widget_click": ("widget_click", {"target": {"imgui": "Arrange", "window": "Arrange options"}}),
}
START = {"printer": checks.KLIPPER, "process": checks.KLIPPER_STD, "filaments": [checks.GENERIC_PLA]}


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

    c.call("app_wait", **{"for": "ready", "timeout_s": 50})
    presets = checks.start(c)
    check("the start is the Klipper printer with one Generic PLA slot", presets == START, presets)
    c.call("view_set", tab="prepare")
    for size in [[6, 6, 6], [4, 9, 5], [7, 3, 5]]:
        c.call("object_select", none=True)
        c.call("object_add_primitive", shape="cube", size=size)
    for o in range(3):
        c.call("object_arrange", fill_object=o, spacing=0, rotate=False)

    for name, (tool, args) in STARTERS.items():
        if name == "widget_click" and c.call("widget_list", query="Arrange options", include_menus=False).get("nodes", 0) == 0:
            c.call("widget_click", target={"gltoolbar": "arrange"})
        out = {}

        def run():
            out["reply"] = attempt(client(), tool, **args)

        starter = threading.Thread(target=run)
        starter.start()
        time.sleep(2.0)
        ok, reply = attempt(c, "object_transform", object=0, translate=[1, 0, 0])
        check(name + ": object_transform refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "object_list", limit=1)
        check(name + ": object_list answers", ok, reply)
        ok, reply = attempt(c, "keyboard_press", keys="Delete")
        check(name + ": keyboard_press Delete refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "widget_click", target={"gltoolbar": "orient"})
        check(name + ": GL toolbar click refused", not ok and "Busy: job" in reply, reply)
        ok, reply = attempt(c, "job_cancel")
        check(name + ": job_cancel stops it", ok and reply.get("was_running") and reply.get("job"), reply)
        starter.join(300)
        ok, reply = out.get("reply", (True, None))
        check(name + ": the call reports it cancelled", not ok and "cancelled" in reply, reply)
        ok, reply = attempt(c, "object_transform", object=0, translate=[-1, 0, 0])
        check(name + ": a change is admitted afterwards", ok, reply)

    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
