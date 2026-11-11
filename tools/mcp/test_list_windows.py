#!/usr/bin/env python3
"""Checks list_windows: every open window, with a ref that screenshot and the GUI tools take.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_list_windows.py

The main window comes first. The Plugins window, which doesn't block the app, is listed with a ref:
a screenshot of that ref captures the window, list_widgets with it as target reads that window alone,
and press_keys Escape sent to it closes it, after which the list leaves it out. Preferences, a modal
dialog, comes last. Action Replay is listed as hidden, without a ref. Exits 0 when every check passes.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def windows():
        return client.call("list_windows").get("items", [])

    def find(title):
        return next((w for w in windows() if w.get("title") == title), None)

    def wait_for(predicate, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.25)
        return predicate()

    client.call("new_project", discard_changes=True)
    client.call("set_view", tab="prepare")

    listed = client.call("list_windows")
    items = listed.get("items", [])
    check("the reply is a page of windows", listed.get("total") == len(items) and listed.get("offset") == 0, listed)
    main_window = items[0] if items else {}
    check("the main window comes first, with a ref", main_window.get("kind") == "main" and main_window.get("ref"), items)
    check("the main window is not modal", main_window.get("modal") is False, main_window)
    check("the main window has a size", main_window.get("width", 0) > 0 and main_window.get("height", 0) > 0, main_window)

    # The Plugins window doesn't block the app.
    client.call("run_action", id="orca_command:open_plugins:orca")
    plugins = wait_for(lambda: find("Plugins"), 15)
    check("the Plugins window is listed", plugins, windows())
    plugins = plugins or {}
    check("it is a window that doesn't block, with a ref", plugins.get("modal") is False and plugins.get("ref"), plugins)
    check("it holds a web page", plugins.get("web") is True, plugins)
    if plugins.get("ref"):
        shot = client.call("screenshot", target=plugins["ref"], include_image=False)
        whole = client.call("screenshot", target="main", include_image=False)
        check("a screenshot of its ref captures it", shot.get("path") and shot.get("width", 0) > 0, shot)
        check("and not the main window", (shot.get("width"), shot.get("height")) != (whole.get("width"), whole.get("height")),
              f"{shot} / {whole}")
        _snap, texts = client.call_with_text("list_widgets", target=plugins["ref"], include_web=False)
        tree = texts[0] if texts else ""
        first = tree.splitlines()[0] if tree else ""
        check("list_widgets with its ref reads that window alone", '"Plugins"' in first and "MainFrame" not in tree, first)
        client.call("press_keys", keys="Escape", target=plugins["ref"])
        check("press_keys Escape on its ref closes it", wait_for(lambda: find("Plugins") is None, 10), windows())

    # A modal dialog comes last.
    prefs = client.call("run_action", id="orca_command:open_preferences:orca")
    modal = wait_for(lambda: find("Preferences"), 15)
    listed = windows()
    check("Preferences is listed as modal, with a ref", modal and modal.get("modal") is True and modal.get("ref"), listed)
    check("the modal dialog comes last", listed and listed[-1].get("title") == "Preferences", listed)
    client.call("answer_dialog", dialog={"title": "Preferences"}, button="close")
    client.await_job(prefs)
    check("Preferences is gone from the list once closed", find("Preferences") is None, windows())

    # Action Replay is the person's own window: listed, but out of reach.
    client.call("run_action", id="orca_command:open_action_replay:orca")
    replay = wait_for(lambda: find("Action Replay"), 15)
    check("Action Replay is listed as hidden", replay and replay.get("hidden") is True, windows())
    check("without a ref", replay is not None and "ref" not in replay, replay)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
