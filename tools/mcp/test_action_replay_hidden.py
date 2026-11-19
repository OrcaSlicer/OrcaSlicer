#!/usr/bin/env python3
"""Checks that the Action Replay window and the Run Script panel stay out of reach of the GUI tools, so a
replay or a script can't act on them.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_action_replay_hidden.py

Each window is first seen up: window_list lists Action Replay as hidden, and the 3D view is shorter once the Run
Script panel docks under it. With Action Replay open: widget_list lists no window or dialog for it, while it lists
the main window as such a line, a window screenshot still works, webview_run_javascript without a target behaves
as it did before the window opened (it must not find a second web view), and keyboard_press without a target goes
to the 3D view, not to a page. With the Run Script panel shown on Prepare: widget_list lists no more web views
than before, webview_run_javascript without a target behaves as before, keyboard_press without a target reaches
the 3D view, a window screenshot works and an agent's script_run finishes. replay_run still runs the smoke test,
and a replay nesting an inline replay_run passes. Exits 0 when every check passes.
"""

import os
import re
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SHOW_LIMIT_S = 10  # from the action that opens a window until it is seen up


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

    def canvas_height():
        """The 3D view's height, from its line in widget_list, or None when it is not listed."""
        _snap, texts = client.call_with_text("widget_list")
        found = re.search(r'canvas "OrcaSceneCanvas"[^\n]*@-?\d+,-?\d+ \d+x(\d+)', "\n".join(texts))
        return int(found.group(1)) if found else None

    def within(limit, condition):
        """Whether `condition()` holds within `limit` s, asked every 0.2 s."""
        deadline = time.monotonic() + limit
        while not condition():
            if time.monotonic() > deadline:
                return False
            time.sleep(0.2)
        return True

    def action_replay_listed():
        return any(w.get("title") == "Action Replay" and w.get("hidden") is True
                   for w in client.call("window_list").get("items", []))

    client.call("project_new", discard_changes=True)
    client.call("view_set", tab="home")
    before = eval_outcome()

    client.call("action_run", id="orca_command:open_action_replay:orca")
    check("Action Replay opens", within(SHOW_LIMIT_S, action_replay_listed),
          f"window_list lists no hidden Action Replay window {SHOW_LIMIT_S} s after the action")
    time.sleep(1.0)  # let its web view load

    snap, texts = client.call_with_text("widget_list")
    tree = texts[0] if texts else ""
    # The File menu's "Action Replay..." item is listed; the window must not be. The main window's line is the
    # control: the tree words a window as its role and its title in quotes.
    check("widget_list lists no Action Replay window", not re.search(r'\b(window|dialog) "Action Replay"', tree))
    check("widget_list still lists the main window", re.search(r'\bwindow "[^"\n]+" \(MainFrame\)', tree))
    check("a window screenshot works", client.call("window_screenshot", target="main").get("path"))
    check("webview_run_javascript without a target behaves as before", eval_outcome() == before, f"{before} / {eval_outcome()}")

    client.call("view_set", tab="prepare")
    client.call("action_run", id="orca_command:open_action_replay:orca")  # raised again, so it has the focus
    # A key that reached a page replies with what the page did; the focus left in a window the tools don't see is
    # no target, so the key goes to the shown 3D view.
    key = client.call("keyboard_press", keys="Escape")
    check("a key without a target never goes to Action Replay's web view",
          "page" not in key and "canvas" in str(key.get("to", "")).lower(), key)

    # The Run Script panel, docked under the 3D view of Prepare.
    client.call("view_set", tab="prepare")
    nodes_before = webview_nodes()
    prepare_eval = eval_outcome()
    height = canvas_height()
    client.call("action_run", id="orca_command:toggle_run_script_panel:orca")
    check("the Run Script panel shows",
          height is not None and within(SHOW_LIMIT_S, lambda: (canvas_height() or height) < height),
          f"the 3D view is {canvas_height()} high {SHOW_LIMIT_S} s after the toggle, {height} before")
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
