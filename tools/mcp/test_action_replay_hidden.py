#!/usr/bin/env python3
"""Checks that the Action Replay window and the Run Script panel stay out of reach of the GUI tools, so a
replay or a script can't act on them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_action_replay_hidden.py

With Action Replay open: widget_list lists no window for it, a window screenshot still works,
webview_run_javascript without a target behaves as it did before the window opened (it must not find a second
web view), and keyboard_press without a target never reaches its web view. With the Run Script panel shown on
Prepare: widget_list lists no more web views than before, webview_run_javascript without a target behaves as
before, keyboard_press without a target reaches the 3D view, a window screenshot works and an agent's script_run
finishes. replay_run still runs the smoke test, and a replay nesting an inline replay_run passes. Exits
0 when every check passes.
"""

import os
import re
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
            return "result: " + str(client.call("webview_run_javascript", code="1 + 1").get("result"))
        except orca_mcp.McpError as e:
            return "error: " + str(e)

    def webview_nodes():
        _snap, texts = client.call_with_text("widget_list")
        return len(re.findall(r"\bwebview\b", texts[0] if texts else ""))

    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="home")
    before = eval_outcome()

    client.call("action_run", id="orca_command:open_action_replay:orca")
    time.sleep(1.0)  # let its web view load

    snap, texts = client.call_with_text("widget_list")
    tree = texts[0] if texts else ""
    # The File menu's "Action Replay..." item is listed; the window must not be.
    check("widget_list lists no Action Replay window", 'window "Action Replay"' not in tree)
    check("widget_list still lists the main window", "(MainFrame)" in tree)
    check("a window screenshot works", client.call("window_screenshot", target="main").get("path"))
    check("webview_run_javascript without a target behaves as before", eval_outcome() == before, f"{before} / {eval_outcome()}")

    client.call("view_set", tab="prepare")
    client.call("action_run", id="orca_command:open_action_replay:orca")  # raised again, so it has the focus
    key = client.call("keyboard_press", keys="Escape")
    check("a key without a target never goes to Action Replay's web view", "webview" not in str(key.get("to", "")).lower(), key)

    # The Run Script panel, docked under the 3D view of Prepare.
    client.call("view_set", tab="prepare")
    nodes_before = webview_nodes()
    prepare_eval = eval_outcome()
    client.call("action_run", id="orca_command:toggle_run_script_panel:orca")
    time.sleep(1.0)  # let its web view load
    check("widget_list lists no node of the Run Script panel", webview_nodes() == nodes_before, f"{nodes_before} / {webview_nodes()}")
    check("webview_run_javascript without a target behaves as before the panel", eval_outcome() == prepare_eval, f"{prepare_eval} / {eval_outcome()}")
    key = client.call("keyboard_press", keys="Escape")
    check("a key without a target reaches the 3D view", "canvas" in str(key.get("to", "")).lower(), key)
    check("a window screenshot works with the panel shown", client.call("window_screenshot", target="main").get("path"))
    python = client.await_job(client.call("script_run", code="print('agent run')"))
    check("an agent's script_run finishes while the panel is shown", python.get("status") == "finished", python)
    client.call("action_run", id="orca_command:toggle_run_script_panel:orca")  # hidden again

    out = tempfile.mkdtemp(prefix="orca-runner-")
    smoke = client.await_job(client.call("replay_run", path=os.path.join(HERE, "smoke_test.json"), out_dir=out))
    check("replay_run still runs the smoke test", smoke.get("passed") is True, smoke.get("failed_step"))

    nested = {
        "orca_replay_version": "0.2.0",
        "name": "nested",
        "steps": [{"tool": "replay_run", "args": {"replay": {"orca_replay_version": "0.2.0", "steps": [{"tool": "app_state"}]}},
                   "expect": {"/passed": {"eq": True}}}],
    }
    result = client.await_job(client.call("replay_run", replay=nested, out_dir=os.path.join(out, "nested")))
    check("a replay nesting an inline replay_run passes", result.get("passed") is True, result)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
