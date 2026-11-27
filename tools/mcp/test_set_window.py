#!/usr/bin/env python3
"""Checks window_list and window_set: every open window, with a ref the GUI tools take, and the size, position
and state of the main window, a window that doesn't block, and a modal dialog, with the read side that
window_list and app_state give.
Its design is the test_set_window entry of tools/mcp/test-entries/windows_web.md.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_set_window.py

window_list is a page of windows, the main window first. The main window is sized twice, the second time from
the normal state, and on Wayland a window screenshot is the size the reply gives. Where positions are known it is
moved, then placed over its display's work area by position and size. It is maximized, given a size while
maximized (which restores it first), minimized, which replies without waiting for a 3D frame, and restored, after
which the 3D view still captures, and minimized while maximized, from which state normal and a size both bring it
back to normal; on Wayland those checks are skipped, as a compositor brings a minimized window back only by
activating the app. A request that
contradicts itself is refused before anything changes. The Plugins window, which doesn't block the app and holds
a web page, is sized and moved by its ref, which window_screenshot captures alone and widget_list reads alone;
keyboard_press Escape sent to it closes it, after which its ref fails, pointing to window_list. Preferences, a
modal dialog listed last, is sized as "dialog" and moved by its {label} locator, each read back from
window_list, and never minimized; on X11 the main window, minimized, restored and raised while it is open, stays
under it in the X server's stacking order. Action Replay, a window the tools reach, is listed with a ref and
closed by Escape sent to it. A check that this window system or display can't run here is reported SKIP, with
why, and doesn't fail the test: it exits 0 when no check fails.
"""

import ctypes
import ctypes.util
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, start, until  # noqa: E402

# How long a call may take before the test reads it as hung.
HANG_S = 30


class _XWindowAttributes(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int), ("y", ctypes.c_int), ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("border_width", ctypes.c_int), ("depth", ctypes.c_int), ("visual", ctypes.c_void_p), ("root", ctypes.c_ulong),
                ("class_", ctypes.c_int), ("bit_gravity", ctypes.c_int), ("win_gravity", ctypes.c_int),
                ("backing_store", ctypes.c_int), ("backing_planes", ctypes.c_ulong), ("backing_pixel", ctypes.c_ulong),
                ("save_under", ctypes.c_int), ("colormap", ctypes.c_ulong), ("map_installed", ctypes.c_int),
                ("map_state", ctypes.c_int), ("all_event_masks", ctypes.c_long), ("your_event_mask", ctypes.c_long),
                ("do_not_propagate_mask", ctypes.c_long), ("override_redirect", ctypes.c_int), ("screen", ctypes.c_void_p)]


def x11_stacking(display):
    """The titles of the mapped top-level windows of an X display, bottom to top, or None when libX11 or the
    display can't be opened. A window manager's frame stands for the titled window it holds."""
    path = ctypes.util.find_library("X11")
    if not path or not display:
        return None
    x = ctypes.CDLL(path)
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XQueryTree.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong),
                             ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)), ctypes.POINTER(ctypes.c_uint)]
    x.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
    x.XFree.argtypes = [ctypes.c_void_p]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    x.XGetWindowAttributes.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(_XWindowAttributes)]
    dpy = x.XOpenDisplay(display.encode())
    if not dpy:
        return None

    def children(window):
        root, parent, n = ctypes.c_ulong(), ctypes.c_ulong(), ctypes.c_uint()
        kids = ctypes.POINTER(ctypes.c_ulong)()
        if not x.XQueryTree(dpy, window, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids), ctypes.byref(n)):
            return []
        out = [kids[i] for i in range(n.value)]
        if kids:
            x.XFree(kids)
        return out

    def viewable(window):
        attrs = _XWindowAttributes()
        return x.XGetWindowAttributes(dpy, window, ctypes.byref(attrs)) and attrs.map_state == 2  # IsViewable

    def title(window, depth=3):
        name = ctypes.c_char_p()
        if x.XFetchName(dpy, window, ctypes.byref(name)) and name.value:
            return name.value.decode(errors="replace")
        for kid in reversed(children(window)) if depth else []:
            found = title(kid, depth - 1)
            if found:
                return found
        return None

    order = [title(w) for w in children(x.XDefaultRootWindow(dpy)) if viewable(w)]
    x.XCloseDisplay(dpy)
    return [t for t in order if t]


def main():
    client, session = orca_mcp.session_client()
    checks = []
    skipped = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def skip(name, why):
        """A check this window system or display can't run here, which doesn't fail the test."""
        skipped.append((name, why))

    def windows():
        return client.call("window_list").get("items", [])

    def find(title):
        return next((w for w in windows() if w.get("title") == title), None)

    def main_entry():
        return next((w for w in windows() if w.get("kind") == "main"), {})

    def reached(fn, what):
        """until()'s value, or None, printed with what was seen, when the 30 s pass first: the check after it fails
        and the test goes on."""
        try:
            return until(fn, what)
        except TimeoutError as e:
            print(e, flush=True)
            return None

    def refusal(**args):
        """The error message of a window_set call that fails, or None when it succeeds."""
        try:
            client.call("window_set", **args)
        except orca_mcp.McpError as e:
            return str(e)
        return None

    started = start(client)
    check("start: the Klipper printer with its default process and filament",
          started == {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}, started)
    state = client.call("app_state")
    wayland = state.get("display_backend") == "wayland"

    # 0. The list at the start, which start() leaves with the main window alone: a page of windows, the main
    # window first.
    listed = client.call("window_list")
    items = listed.get("items", [])
    check("window_list: the reply is a page of windows", listed.get("total") == len(items) and listed.get("offset") == 0, listed)
    first = items[0] if items else {}
    check("window_list: the main window comes first, with a ref", first.get("kind") == "main" and first.get("ref"), items)
    check("window_list: the main window is not modal", first.get("modal") is False, first)
    check("window_list: the main window has a size", first.get("width", 0) > 0 and first.get("height", 0) > 0, first)

    # 1. Size.
    sized = client.call("window_set", size=[1400, 900])
    check("size: the reply has the size asked for", sized.get("size") == [1400, 900], sized)
    check("size: the window is in its normal state", sized.get("state") == "normal", sized)
    entry = main_entry()
    check("size: window_list reports it", (entry.get("width"), entry.get("height")) == (1400, 900), entry)
    # A second size from the normal state. Taking a normal window out of a maximize it isn't in makes a
    # Wayland compositor send it its current size, which replaced the one asked for.
    again = client.call("window_set", size=[1300, 820])
    check("a second size: the reply has it", again.get("size") == [1300, 820], again)
    entry = main_entry()
    check("a second size: window_list reports it", (entry.get("width"), entry.get("height")) == (1300, 820), entry)
    if wayland:
        # The size is the window as the compositor shows it, which a window screenshot captures; the shadow
        # GTK draws around it is not part of it.
        shot = client.call("window_screenshot", target="main", include_image=False)
        scale = again.get("scale", 1)
        check("a second size: a window screenshot is that size", (shot.get("width"), shot.get("height")) ==
              (round(1300 * scale), round(820 * scale)), f"{shot} / {again}")

    # 2. Position.
    if wayland:
        skip("position: the window is moved and placed over its work area", "Wayland takes no position")
        window = client.call("app_state").get("window", {})
        check("position: app_state reports none on Wayland", "position" not in window, window)
    else:
        area = sized.get("display", {})
        client.call("window_set", position=[40, 30])
        window = client.call("app_state").get("window", {})
        # A top bar holds windows below it, as GNOME Shell's does at 32, so the window goes no higher than the top
        # of the work area.
        check("position: app_state reports it", window.get("position") == [40, max(30, area.get("y", 0))],
              f"{window} / {area}")
        # The display's work area, given as a place and a size, is taken as it is: a window manager keeps a
        # window that fits it there, and with none the window goes exactly there.
        filled = client.call("window_set", position=[area.get("x", 0), area.get("y", 0)],
                             size=[area.get("width", 0), area.get("height", 0)])
        check("position and size: the window fills the display's work area",
              filled.get("position") == [area.get("x"), area.get("y")] and filled.get("size") == [area.get("width"), area.get("height")],
              f"{filled} / {area}")

    # 3. Maximize.
    maximized = client.call("window_set", state="maximized")
    check("maximize: the reply says maximized", maximized.get("state") == "maximized", maximized)

    # 4. A size while maximized restores the window first.
    resized = client.call("window_set", size=[1300, 850])
    check("a size while maximized: the window is restored", resized.get("state") == "normal", resized)
    check("a size while maximized: and given the size", resized.get("size") == [1300, 850], resized)

    # 5. Minimize and restore, neither waiting for a 3D frame a minimized window never draws. A minimized
    # window comes back on request on X11, where a window manager un-maps and maps it; a Wayland compositor
    # brings it back only by activating the app, which it allows only while the app may take the focus, so a
    # person restores it from the taskbar there and the checks that need it back are skipped.
    if wayland:
        why = ("a Wayland compositor brings a minimized window back only by activating it, which it grants only "
               "while the app may take the focus")
        skip("minimize and restore", why)
        skip("a window minimized while maximized", why)
    else:
        # A wait for a 3D frame runs to its 3 s and then replies with a note that the 3D view did not draw.
        try:
            minimized = client.call("window_set", _timeout=HANG_S, state="minimized")
        except OSError as e:  # the client's timeout
            minimized = {"error": f"no reply within {HANG_S} s: {e}"}
        check("minimize: replies without waiting for a 3D frame", "error" not in minimized and "note" not in minimized,
              minimized)
        check("minimize: the reply says minimized", minimized.get("state") == "minimized", minimized)
        restored = client.call("window_set", state="normal")
        check("restore: the reply says normal", restored.get("state") == "normal", restored)
        try:
            shot = client.call("window_screenshot", target="canvas", include_image=False)
            check("restore: the 3D view captures", shot.get("width", 0) > 0, shot)
        except orca_mcp.McpError as e:
            check("restore: the 3D view captures", False, str(e))

        # 5b. A window minimized while maximized comes back to normal, not to maximized: through state normal,
        # and through a size, which restores it first.
        client.call("window_set", state="maximized")
        client.call("window_set", state="minimized")
        normal = client.call("window_set", state="normal")
        check("maximized, minimized, then normal: the reply says normal", normal.get("state") == "normal", normal)
        check("without a note that the window manager kept the request waiting", "note" not in normal, normal)
        # On macOS the window comes back from the Dock zoomed and is un-zoomed once it is back, so it must not
        # turn maximized again after the reply. Nothing tells when the window manager is done with a window, so
        # window_list is read every 0.1 s through the second after the reply.
        states, end = [], time.monotonic() + 1
        while time.monotonic() < end:
            states.append(main_entry().get("state"))
            time.sleep(0.1)
        check("and window_list reads it normal throughout the second after", set(states) == {"normal"}, states)
        client.call("window_set", state="maximized")
        client.call("window_set", state="minimized")
        sized = client.call("window_set", size=[1350, 880])
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
    client.call("action_run", id="orca_command:open_plugins:orca")
    plugins = reached(lambda: find("Plugins"), "the Plugins window is listed") or {}
    check("the Plugins window is listed with a ref", plugins.get("ref"), plugins)
    check("Plugins: it is a window that doesn't block", plugins.get("modal") is False, plugins)
    check("Plugins: it holds a web page", plugins.get("web") is True, plugins)
    ref = plugins.get("ref")
    if ref:
        # Wayland takes no position.
        placed = client.call("window_set", target=ref, size=[900, 700], **({} if wayland else {"position": [100, 100]}))
        # The window's minimum height, from its layout, can be above 700 (715 at 100 % scaling): the size is
        # then held to it, and the reply lists size as adjusted.
        size = placed.get("size") or [0, 0]
        check("Plugins: the reply has the size, or its minimum height with size adjusted",
              size == [900, 700] or (size[0] == 900 and size[1] > 700 and "size" in placed.get("adjusted", [])), placed)
        # The reply's fields are window_list's entry, which has no place on Wayland, where none is given or known.
        check("Plugins: the reply has the position",
              "position" not in placed if wayland else placed.get("position") == [100, 100], placed)
        entry = find("Plugins") or {}
        got = (entry.get("width"), entry.get("height")) if wayland else (entry.get("x"), entry.get("y"), entry.get("width"), entry.get("height"))
        want = tuple(size) if wayland else tuple(placed.get("position", []) + size)
        check("Plugins: window_list reports the place and size the reply gives", got == want, f"{entry} / {placed}")
        # The other tools that take a window's ref.
        shot = client.call("window_screenshot", target=ref, include_image=False)
        whole = client.call("window_screenshot", target="main", include_image=False)
        check("Plugins: a screenshot of its ref captures it", shot.get("path") and shot.get("width", 0) > 0, shot)
        check("Plugins: and not the main window", (shot.get("width"), shot.get("height")) != (whole.get("width"), whole.get("height")),
              f"{shot} / {whole}")
        _snap, texts = client.call_with_text("widget_list", target=ref, include_web=False)
        tree = texts[0] if texts else ""
        top = tree.splitlines()[0] if tree else ""
        check("Plugins: widget_list with its ref reads that window alone", '"Plugins"' in top and "MainFrame" not in tree, top)
        client.call("keyboard_press", keys="Escape", target=ref)
        check("Plugins: keyboard_press Escape closes it",
              reached(lambda: find("Plugins") is None, "the Plugins window is gone"), windows())

        # 8. A ref to a window that has closed.
        error = refusal(**{"target": ref, "raise": True})
        check("a closed window's ref is refused", error is not None, error)
        check("pointing to window_list", error is not None and "window_list" in error, error)

    # 9. A modal dialog.
    prefs = client.call("action_run", id="orca_command:open_preferences:orca")
    modal = reached(lambda: find("Preferences"), "Preferences is listed")
    listed = windows()
    check("Preferences is listed as modal, with a ref", modal and modal.get("modal") is True and modal.get("ref"), listed)
    check("the modal dialog comes last", listed and listed[-1].get("title") == "Preferences", listed)
    if modal:
        dialog_size = [0, 0]
        try:
            dialog = client.call("window_set", target="dialog", size=[1000, 760])
            # As for Plugins, a minimum height above 760 holds the size to it, and the reply lists size as adjusted.
            dialog_size = dialog.get("size") or [0, 0]
            entry = find("Preferences") or {}
            check("dialog: window_set sizes the topmost modal dialog",
                  (dialog_size == [1000, 760] or (dialog_size[0] == 1000 and dialog_size[1] > 760 and
                                                  "size" in dialog.get("adjusted", [])))
                  and [entry.get("width"), entry.get("height")] == dialog_size, f"{dialog} / {entry}")
            check("dialog: the reply's kind is dialog", dialog.get("kind") == "dialog", dialog)
        except orca_mcp.McpError as e:
            check("dialog: window_set sizes the topmost modal dialog", False, str(e))
        # A replay records the dialog by this locator. Wayland takes no position, so the call sizes it there.
        try:
            moved = client.call("window_set", target={"label": "Preferences"},
                                **({"size": [1000, 760]} if wayland else {"position": [60, 60]}))
            entry = find("Preferences") or {}
            if wayland:
                check("dialog: a {label} locator finds it",
                      [entry.get("width"), entry.get("height")] == moved.get("size"), f"{moved} / {entry}")
            else:
                check("dialog: a {label} locator finds it",
                      moved.get("position") == [60, 60] and
                      [entry.get("x"), entry.get("y"), entry.get("width"), entry.get("height")] == [60, 60] + dialog_size,
                      f"{moved} / {entry}")
            check("dialog: the reply's title is Preferences", moved.get("title") == "Preferences", moved)
        except orca_mcp.McpError as e:
            check("dialog: a {label} locator finds it", False, str(e))
        error = refusal(target="dialog", state="minimized")
        check("dialog: a modal dialog never minimizes", error is not None, error)
        # The main window restored and raised behind the dialog stays under it, as X11 captures read the
        # screen: a window manager keeps a dialog above its window, and with none the app does. Wayland has
        # no stacking order to read, and a minimized window there comes back only by activation.
        if wayland:
            skip("dialog: the main window, restored and raised, stays under it",
                 "a Wayland session has no X server stacking order to read")
        else:
            client.call("window_set", state="minimized")
            client.call("window_set", state="normal")
            client.call("window_set", **{"raise": True})
            order = x11_stacking(session.get("display") or os.environ.get("DISPLAY"))
            if order is None:
                skip("dialog: the main window, restored and raised, stays under it",
                     "the X server's stacking order can't be read here")
            else:
                title = main_entry().get("title")
                check("dialog: the main window, restored and raised, stays under it",
                      title in order and "Preferences" in order and order.index(title) < order.index("Preferences"),
                      order)
        client.call("dialog_answer", dialog={"title": "Preferences"}, button="close")
        client.await_job(prefs)
        check("Preferences closes", find("Preferences") is None, windows())

    # 10. Action Replay is a window the tools reach: listed with a ref, and closed by a key sent to it.
    client.call("action_run", id="orca_command:open_action_replay:orca")
    replay = reached(lambda: find("Action Replay"), "Action Replay is listed")
    check("Action Replay is listed, not hidden", replay is not None and "hidden" not in replay, windows())
    check("Action Replay: with a ref, not modal, showing a web page",
          replay is not None and bool(replay.get("ref")) and replay.get("modal") is False and replay.get("web") is True, replay)
    if replay is not None and replay.get("ref"):
        client.call("keyboard_press", keys="Escape", target=replay["ref"])
        check("Action Replay: keyboard_press Escape on its ref closes it",
              reached(lambda: find("Action Replay") is None, "Action Replay is gone"), windows())

    # 11. The read side.
    listed = [w for w in windows() if w.get("ref")]
    check("window_list: each window with a ref has state and resizable",
          listed and all(w.get("state") in ("normal", "maximized", "minimized") and isinstance(w.get("resizable"), bool) for w in listed),
          listed)
    displays = client.call("app_state").get("displays", [])
    check("app_state: displays are listed", len(displays) > 0, displays)
    check("app_state: exactly one is primary", sum(1 for d in displays if d.get("primary") is True) == 1, displays)
    check("app_state: each display has its work area and scale",
          all(all(k in d for k in ("index", "x", "y", "width", "height", "scale")) for d in displays), displays)

    # 12. The flat form window_list reports.
    flat = client.call("window_set", width=1500, height=950)
    check("flat form: width and height are read as size", flat.get("size") == [1500, 950], flat)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    for name, why in skipped:
        print(f"SKIP {name}: {why}")
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed" +
          (f", {len(skipped)} skipped" if skipped else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
