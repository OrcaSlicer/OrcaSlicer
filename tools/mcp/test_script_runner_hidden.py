#!/usr/bin/env python3
"""Checks that the Run Script dialog stays out of reach of the GUI tools, so a script can't act on it.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_script_runner_hidden.py

With the dialog open: ui_snapshot lists no window for it, a window screenshot still works,
webview_eval without a target behaves as it did before the dialog opened (it must not find a second
web view), and ui_key without a target goes to the 3D view. run_script still runs the smoke test, and
a script nesting an inline run_script passes. Exits 0 when every check passes.
"""

import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def eval_outcome():
        try:
            return "result: " + str(client.call("webview_eval", script="1 + 1").get("result"))
        except orca_mcp.McpError as e:
            return "error: " + str(e)

    client.call("new_project", discard_changes=True)
    client.call("set_view", tab="home")
    before = eval_outcome()

    client.call("run_action", id="orca_command:open_script_runner:orca")
    time.sleep(1.0)  # let its web view load

    snap, texts = client.call_with_text("ui_snapshot")
    tree = texts[0] if texts else ""
    # The File menu's "Run Script..." item is listed; the dialog's window must not be.
    check("ui_snapshot lists no Run Script window", 'window "Run Script"' not in tree)
    check("ui_snapshot still lists the main window", "(MainFrame)" in tree)
    check("a window screenshot works", client.call("screenshot", target="window").get("path"))
    check("webview_eval without a target behaves as before", eval_outcome() == before, f"{before} / {eval_outcome()}")

    client.call("set_view", tab="prepare")
    client.call("run_action", id="orca_command:open_script_runner:orca")  # raised again, so it has the focus
    key = client.call("ui_key", keys="Escape")
    check("a key without a target never goes to the runner's web view", "webview" not in str(key.get("to", "")).lower(), key)

    out = tempfile.mkdtemp(prefix="orca-runner-")
    smoke = client.await_job(client.call("run_script", path=os.path.join(HERE, "smoke_test.json"), out_dir=out))
    check("run_script still runs the smoke test", smoke.get("passed") is True, smoke.get("failed_step"))

    nested = {
        "orca_script": 1,
        "name": "nested",
        "steps": [{"tool": "run_script", "args": {"script": {"orca_script": 1, "steps": [{"tool": "get_state"}]}},
                   "expect": {"/passed": {"eq": True}}}],
    }
    result = client.await_job(client.call("run_script", script=nested, out_dir=os.path.join(out, "nested")))
    check("a script nesting an inline run_script passes", result.get("passed") is True, result)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
