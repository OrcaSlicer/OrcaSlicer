#!/usr/bin/env python3
"""Checks set_window: the size, position and state of the main window, a window that doesn't block, and a
modal dialog, with the read side that list_windows and get_app_state give.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_set_window.py

The main window is sized, moved, maximized, given a size while maximized (which restores it first),
minimized and restored, after which the 3D view still captures, and minimized while maximized, from which
state normal and a size both bring it back to normal. A request that contradicts itself is
refused before anything changes. The Plugins window is sized and moved by its ref, which fails, pointing to
list_windows, once the window has closed. Preferences, a modal dialog, is sized as "dialog" and moved by its
{label} locator, and never minimized. set_view refuses a window_size, pointing to set_window. Exits 0
when every check passes.
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

    def main_entry():
        return next((w for w in windows() if w.get("kind") == "main"), {})

    def wait_for(predicate, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.25)
        return predicate()

    def refusal(**args):
        """The error message of a set_window call that fails, or None when it succeeds."""
        try:
            client.call("set_window", **args)
        except orca_mcp.McpError as e:
            return str(e)
        return None

    client.call("new_project", discard_changes=True)
    client.call("set_view", tab="prepare")
    state = client.call("get_app_state")
    wayland = state.get("display_backend") == "wayland"

    # 1. Size.
    sized = client.call("set_window", size=[1400, 900])
    check("size: the reply has the size asked for", sized.get("size") == [1400, 900], sized)
    check("size: the window is in its normal state", sized.get("state") == "normal", sized)
    entry = main_entry()
    check("size: list_windows reports it", (entry.get("width"), entry.get("height")) == (1400, 900), entry)

    # 2. Position.
    if wayland:
        print("NOTE position: skipped, as Wayland neither reports nor accepts positions")
    else:
        client.call("set_window", position=[40, 30])
        window = client.call("get_app_state").get("window", {})
        check("position: get_app_state reports it", window.get("position") == [40, 30], window)

    # 3. Maximize.
    maximized = client.call("set_window", state="maximized")
    check("maximize: the reply says maximized", maximized.get("state") == "maximized", maximized)

    # 4. A size while maximized restores the window first.
    resized = client.call("set_window", size=[1300, 850])
    check("a size while maximized: the window is restored", resized.get("state") == "normal", resized)
    check("a size while maximized: and given the size", resized.get("size") == [1300, 850], resized)

    # 5. Minimize and restore, neither waiting for a 3D frame a minimized window never draws.
    start = time.time()
    minimized = client.call("set_window", state="minimized")
    took = time.time() - start
    check("minimize: returns within 5 s", took < 5, f"{took:.1f} s: {minimized}")
    check("minimize: the reply says minimized", minimized.get("state") == "minimized", minimized)
    restored = client.call("set_window", state="normal")
    check("restore: the reply says normal", restored.get("state") == "normal", restored)
    try:
        shot = client.call("screenshot", target="canvas", include_image=False)
        check("restore: the 3D view captures", shot.get("width", 0) > 0, shot)
    except orca_mcp.McpError as e:
        check("restore: the 3D view captures", False, str(e))

    # 5b. A window minimized while maximized comes back to normal, not to maximized: through state normal,
    # and through a size, which restores it first.
    client.call("set_window", state="maximized")
    client.call("set_window", state="minimized")
    normal = client.call("set_window", state="normal")
    check("maximized, minimized, then normal: the reply says normal", normal.get("state") == "normal", normal)
    check("without a note that the window manager kept the request waiting", "note" not in normal, normal)
    client.call("set_window", state="maximized")
    client.call("set_window", state="minimized")
    sized = client.call("set_window", size=[1350, 880])
    check("maximized, minimized, then a size: the reply says normal", sized.get("state") == "normal", sized)
    check("and has the size", sized.get("size") == [1350, 880], sized)

    # 6. A contradiction is refused before anything changes.
    before = main_entry()
    error = refusal(state="maximized", size=[800, 600])
    after = main_entry()
    check("a contradiction is refused", error is not None, error)
    check("and the window keeps its size", (before.get("width"), before.get("height")) == (after.get("width"), after.get("height")),
          f"{before} / {after}")

    # 7. A window that doesn't block, by its ref.
    client.call("run_action", id="orca_command:open_plugins:orca")
    plugins = wait_for(lambda: find("Plugins"), 15) or {}
    check("the Plugins window is listed with a ref", plugins.get("ref"), plugins)
    ref = plugins.get("ref")
    if ref:
        # Wayland takes no position.
        placed = client.call("set_window", target=ref, size=[900, 700], **({} if wayland else {"position": [100, 100]}))
        # The window's minimum height, from its layout, can be above 700 (715 at 100 % scaling): the size is
        # then held to it, and the reply lists size as adjusted.
        size = placed.get("size") or [0, 0]
        check("Plugins: the reply has the size, or its minimum height with size adjusted",
              size == [900, 700] or (size[0] == 900 and size[1] > 700 and "size" in placed.get("adjusted", [])), placed)
        check("Plugins: the reply has the position", wayland or placed.get("position") == [100, 100], placed)
        entry = find("Plugins") or {}
        got = (entry.get("width"), entry.get("height")) if wayland else (entry.get("x"), entry.get("y"), entry.get("width"), entry.get("height"))
        want = tuple(size) if wayland else tuple(placed.get("position", []) + size)
        check("Plugins: list_windows reports the place and size the reply gives", got == want, f"{entry} / {placed}")
        client.call("press_keys", keys="Escape", target=ref)
        check("Plugins: press_keys Escape closes it", wait_for(lambda: find("Plugins") is None, 10), windows())

        # 8. A ref to a window that has closed.
        error = refusal(**{"target": ref, "raise": True})
        check("a closed window's ref is refused", error is not None, error)
        check("pointing to list_windows", error is not None and "list_windows" in error, error)

    # 9. A modal dialog.
    prefs = client.call("run_action", id="orca_command:open_preferences:orca")
    modal = wait_for(lambda: find("Preferences"), 15)
    check("Preferences is open", modal, windows())
    if modal:
        try:
            dialog = client.call("set_window", target="dialog", size=[1000, 760])
            check("dialog: set_window sizes the topmost modal dialog", True)
            check("dialog: the reply's kind is dialog", dialog.get("kind") == "dialog", dialog)
        except orca_mcp.McpError as e:
            check("dialog: set_window sizes the topmost modal dialog", False, str(e))
        # A replay records the dialog by this locator. Wayland takes no position, so the call sizes it there.
        try:
            moved = client.call("set_window", target={"label": "Preferences"},
                                **({"size": [1000, 760]} if wayland else {"position": [60, 60]}))
            check("dialog: a {label} locator finds it", True)
            check("dialog: the reply's title is Preferences", moved.get("title") == "Preferences", moved)
        except orca_mcp.McpError as e:
            check("dialog: a {label} locator finds it", False, str(e))
        error = refusal(target="dialog", state="minimized")
        check("dialog: a modal dialog never minimizes", error is not None, error)
        client.call("answer_dialog", dialog={"title": "Preferences"}, button="close")
        client.await_job(prefs)
        check("Preferences closes", find("Preferences") is None, windows())

    # 10. The read side.
    listed = [w for w in windows() if w.get("ref")]
    check("list_windows: each window with a ref has state and resizable",
          listed and all(w.get("state") in ("normal", "maximized", "minimized") and isinstance(w.get("resizable"), bool) for w in listed),
          listed)
    displays = client.call("get_app_state").get("displays", [])
    check("get_app_state: displays are listed", len(displays) > 0, displays)
    check("get_app_state: exactly one is primary", sum(1 for d in displays if d.get("primary") is True) == 1, displays)
    check("get_app_state: each display has its work area and scale",
          all(all(k in d for k in ("index", "x", "y", "width", "height", "scale")) for d in displays), displays)

    # 11. The flat form list_windows reports.
    flat = client.call("set_window", width=1500, height=950)
    check("flat form: width and height are read as size", flat.get("size") == [1500, 950], flat)

    # 12. set_view sizes no window: its window_size is refused with a pointer to set_window.
    try:
        client.call("set_view", window_size=[800, 600])
        error = None
    except orca_mcp.McpError as e:
        error = str(e)
    check("set_view refuses window_size", error is not None, error)
    check("pointing to set_window", error is not None and "set_window" in error, error)

    # 13. Clean-up.
    client.call("set_window", size=[1920, 1200])

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
