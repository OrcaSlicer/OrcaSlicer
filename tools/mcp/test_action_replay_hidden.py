#!/usr/bin/env python3
"""Checks that the Action Replay window and the Run Script panel stay out of reach of the GUI tools, so a
replay or a script can't act on them.

    tools/mcp/orca_mcp.py launch --python-scripts
    tools/mcp/test_action_replay_hidden.py

With Action Replay open: list_widgets lists no window for it, a window screenshot still works,
run_javascript without a target behaves as it did before the window opened (it must not find a second
web view), and press_keys without a target never reaches its web view. With the Run Script panel shown on
Prepare: list_widgets lists no more web views than before, run_javascript without a target behaves as
before, press_keys without a target reaches the 3D view, a window screenshot works and an agent's run_script
finishes. run_replay still runs the smoke test, and a replay nesting an inline run_replay passes. Exits
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
            return "result: " + str(client.call("run_javascript", code="1 + 1").get("result"))
        except orca_mcp.McpError as e:
            return "error: " + str(e)

    def webview_nodes():
        _snap, texts = client.call_with_text("list_widgets")
        return len(re.findall(r"\bwebview\b", texts[0] if texts else ""))

    client.call("new_project", discard_changes=True)
    client.call("set_view", tab="home")
    before = eval_outcome()

    client.call("run_action", id="orca_command:open_action_replay:orca")
    time.sleep(1.0)  # let its web view load

    snap, texts = client.call_with_text("list_widgets")
    tree = texts[0] if texts else ""
    # The File menu's "Action Replay..." item is listed; the window must not be.
    check("list_widgets lists no Action Replay window", 'window "Action Replay"' not in tree)
    check("list_widgets still lists the main window", "(MainFrame)" in tree)
    check("a window screenshot works", client.call("screenshot", target="main").get("path"))
    check("run_javascript without a target behaves as before", eval_outcome() == before, f"{before} / {eval_outcome()}")

    client.call("set_view", tab="prepare")
    client.call("run_action", id="orca_command:open_action_replay:orca")  # raised again, so it has the focus
    key = client.call("press_keys", keys="Escape")
    check("a key without a target never goes to Action Replay's web view", "webview" not in str(key.get("to", "")).lower(), key)

    # The Run Script panel, docked under the 3D view of Prepare.
    client.call("set_view", tab="prepare")
    nodes_before = webview_nodes()
    prepare_eval = eval_outcome()
    client.call("run_action", id="orca_command:toggle_run_script:orca")
    time.sleep(1.0)  # let its web view load
    check("list_widgets lists no node of the Run Script panel", webview_nodes() == nodes_before, f"{nodes_before} / {webview_nodes()}")
    check("run_javascript without a target behaves as before the panel", eval_outcome() == prepare_eval, f"{prepare_eval} / {eval_outcome()}")
    key = client.call("press_keys", keys="Escape")
    check("a key without a target reaches the 3D view", "canvas" in str(key.get("to", "")).lower(), key)
    check("a window screenshot works with the panel shown", client.call("screenshot", target="main").get("path"))
    python = client.await_job(client.call("run_script", code="print('agent run')"))
    check("an agent's run_script finishes while the panel is shown", python.get("status") == "finished", python)
    client.call("run_action", id="orca_command:toggle_run_script:orca")  # hidden again

    out = tempfile.mkdtemp(prefix="orca-runner-")
    smoke = client.await_job(client.call("run_replay", path=os.path.join(HERE, "smoke_test.json"), out_dir=out))
    check("run_replay still runs the smoke test", smoke.get("passed") is True, smoke.get("failed_step"))

    nested = {
        "orca_replay": 1,
        "name": "nested",
        "steps": [{"tool": "run_replay", "args": {"replay": {"orca_replay": 1, "steps": [{"tool": "get_app_state"}]}},
                   "expect": {"/passed": {"eq": True}}}],
    }
    result = client.await_job(client.call("run_replay", replay=nested, out_dir=os.path.join(out, "nested")))
    check("a replay nesting an inline run_replay passes", result.get("passed") is True, result)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
