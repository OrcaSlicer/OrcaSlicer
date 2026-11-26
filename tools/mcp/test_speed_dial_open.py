#!/usr/bin/env python3
"""Checks that Space opens the Speed Dial right after a switch to each page with a 3D view, and that KWin draws
no title bar on the dial.
Its design is the test_speed_dial_open entry of tools/mcp/test-entries/windows_web.md.

    tools/mcp/orca_mcp.py launch [--cad]
    tools/mcp/test_speed_dial_open.py

Each page is reached from Home by a click on its tab, as a person switches pages: Prepare first, while the
plater shows the 3D view start() leaves it on, then Design when the Design tab is on, and Preview. The page's
3D view must then hold the keyboard focus, as a focused sidebar control keeps Space for itself, and Space sent
to the focused window must open the dial. The view also takes the focus when the pointer enters it, so before
each tab click the test moves the system pointer to the primary display's top-left corner, (0, 0), and reads
it back there: the test first places the main window, at start()'s 1600x900, at the top left of the primary
display's work area, so that corner is its top bar or lies outside it, never in the 3D view below the tab bar. A
pointer that does not read back at (0, 0) fails the page's focus check. On a native Wayland display, where an app
can neither move the pointer nor read where it is, the focus checks are reported SKIP. Where KWin answers on the
display's D-Bus (the private one of a `--headless` launch, else the caller's), the frame KWin reports for the dial
must be the dial's own size: a title bar of KWin's makes the frame taller. Exits 0 when every check passes.
"""

import ctypes
import ctypes.util
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, speed_dial, start, until  # noqa: E402


def park_pointer(display):
    """Moves the system pointer to (0, 0), the primary display's top-left corner, and returns where the system then
    reads it, as (x, y), or a string saying why it could not be moved or read."""
    if os.name == "nt":
        from ctypes import wintypes
        user32 = ctypes.windll.user32
        if not user32.SetCursorPos(0, 0):
            return f"SetCursorPos failed ({ctypes.GetLastError()})"
        point = wintypes.POINT()
        if not user32.GetCursorPos(ctypes.byref(point)):
            return f"GetCursorPos failed ({ctypes.GetLastError()})"
        return (point.x, point.y)
    if sys.platform == "darwin":
        class CGPoint(ctypes.Structure):
            _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]
        graphics = ctypes.CDLL(ctypes.util.find_library("CoreGraphics")
                               or "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        foundation = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
        graphics.CGWarpMouseCursorPosition.argtypes = [CGPoint]
        graphics.CGEventCreate.restype = ctypes.c_void_p
        graphics.CGEventCreate.argtypes = [ctypes.c_void_p]
        graphics.CGEventGetLocation.restype = CGPoint
        graphics.CGEventGetLocation.argtypes = [ctypes.c_void_p]
        foundation.CFRelease.argtypes = [ctypes.c_void_p]
        if graphics.CGWarpMouseCursorPosition(CGPoint(0, 0)) != 0:
            return "CGWarpMouseCursorPosition failed"
        event = graphics.CGEventCreate(None)
        if not event:
            return "CGEventCreate failed"
        location = graphics.CGEventGetLocation(event)
        foundation.CFRelease(event)
        return (round(location.x), round(location.y))
    path = ctypes.util.find_library("X11")
    if not path or not display:
        return "libX11 or the X display is not found"
    x = ctypes.CDLL(path)
    x.XOpenDisplay.restype = ctypes.c_void_p
    x.XOpenDisplay.argtypes = [ctypes.c_char_p]
    x.XDefaultRootWindow.restype = ctypes.c_ulong
    x.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
    x.XWarpPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
                               ctypes.c_uint, ctypes.c_uint, ctypes.c_int, ctypes.c_int]
    x.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    ulong, cint, cuint = ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_uint)
    x.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ulong, ulong, cint, cint, cint, cint, cuint]
    x.XCloseDisplay.argtypes = [ctypes.c_void_p]
    dpy = x.XOpenDisplay(display.encode())
    if not dpy:
        return f"the X display {display} does not open"
    try:
        root = x.XDefaultRootWindow(dpy)
        x.XWarpPointer(dpy, 0, root, 0, 0, 0, 0, 0, 0)
        x.XSync(dpy, 0)
        root_ret, child = ctypes.c_ulong(), ctypes.c_ulong()
        rx, ry, wx, wy, mask = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint()
        if not x.XQueryPointer(dpy, root, ctypes.byref(root_ret), ctypes.byref(child), ctypes.byref(rx), ctypes.byref(ry),
                               ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(mask)):
            return "XQueryPointer found the pointer on another screen"
        return (rx.value, ry.value)
    finally:
        x.XCloseDisplay(dpy)


def kwin_frames(session):
    """[(caption, width, height)] of the app's windows as KWin frames them, or None when no KWin answers."""
    env = dict(os.environ)
    bus = (session.get("display_env") or {}).get("DBUS_SESSION_BUS_ADDRESS")
    if bus:
        env["DBUS_SESSION_BUS_ADDRESS"] = bus

    def gdbus(path, method, *args):
        try:
            r = subprocess.run(["gdbus", "call", "--session", "--dest", "org.kde.KWin", "--object-path", path,
                                "--method", method, *args], env=env, capture_output=True, text=True, timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            return None
        return r.stdout if r.returncode == 0 else None

    # KWin's window runner finds windows by caption or application, the dial (which has no caption) by the latter.
    found = gdbus("/WindowsRunner", "org.kde.krunner1.Match", "orca")
    if found is None:
        return None
    frames = []
    for uuid in re.findall(r"'0_(\{[^}]+\})'", found):
        info = gdbus("/KWin", "org.kde.KWin.getWindowInfo", uuid) or ""
        pid = re.search(r"'pid': <(\d+)>", info)
        if pid and int(pid.group(1)) == session["pid"]:
            caption = re.search(r"'caption': <'(.*?)'>", info)
            size = [float(re.search(rf"'{k}': <([\d.]+)>", info).group(1)) for k in ("width", "height")]
            frames.append((caption.group(1) if caption else "", *size))
    return frames


def main():
    client, session = orca_mcp.session_client()
    checks = []
    skipped = []
    wayland = client.call("app_state").get("display_backend") == "wayland"
    display = session.get("display") or os.environ.get("DISPLAY")
    parked = []  # where the pointer read back after each move before a tab click of the current switch

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def park():
        """Moves the pointer off the 3D view, to (0, 0), before a tab click, and keeps where it read back."""
        if not wayland:
            parked.append(park_pointer(display))

    def reached(fn, what):
        """until()'s value, or None, printed with what was seen, when the 30 s pass first: the check after it fails
        and the test goes on."""
        try:
            return until(fn, what)
        except TimeoutError as e:
            print(e, flush=True)
            return None

    def on_tab(tab):
        return reached(lambda: client.call("app_state")["tab"] == tab, f"app_state's tab is {tab}")

    def tree():
        _, texts = client.call_with_text("widget_list", max_nodes=3000)
        return "\n".join(texts)

    def switch_to(label, tab):
        """Clicks Home's tab (the first, with no label), then, once app_state's tab is home, the tab `label`, and
        returns whether app_state's tab became `tab`."""
        tabs = tree()
        home = re.search(r"\[(w\d+)\] tab \[", tabs)
        page = re.search(rf'\[(w\d+)\] tab "{re.escape(label)}"', tabs)
        parked.clear()
        park()
        client.call("widget_click", target=home.group(1))
        if not on_tab("home"):
            return False
        park()
        client.call("widget_click", target=page.group(1))
        return on_tab(tab)

    started = start(client)
    check("start: the Klipper printer with its default process and filament",
          started == {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}, started)
    # The main window at the top left of the primary display's work area keeps (0, 0) off the 3D view; Wayland takes
    # no position, and its window keeps start()'s size alone.
    if not wayland:
        primary = next(d for d in client.call("app_state")["displays"] if d.get("primary"))
        client.call("window_set", position=[primary["x"], primary["y"]])

    pages = [("Prepare", "prepare")]
    if client.call("app_state").get("design_tab"):
        pages.append(("Design", "design"))
    pages.append(("Preview", "preview"))

    frame_checked = False
    for label, tab in pages:
        check(f"{label}: the tab is shown", switch_to(label, tab))
        focused = [line.strip() for line in tree().splitlines() if "[focused]" in line]
        if wayland:
            skipped.append((f"{label}: its 3D view holds the keyboard focus",
                            "on a native Wayland display an app can neither move the pointer off the view nor read "
                            "where it is"))
        else:
            check(f"{label}: its 3D view holds the keyboard focus",
                  all(p == (0, 0) for p in parked) and focused and "name=OrcaSceneCanvas" in focused[0],
                  f"the pointer read back at {parked} after each move to (0, 0); focused: {focused}")
        client.call("keyboard_press", keys="Space")
        opened = reached(lambda: speed_dial(client), "the Speed Dial is listed")
        check(f"{label}: Space opens the Speed Dial", opened)
        if not opened:
            continue
        if not frame_checked:
            frame_checked = True
            frames = kwin_frames(session)
            frame = next((f for f in frames or [] if f[0] == ""), None)
            if frames is None:
                print("skip  KWin's frame of the dial: no KWin answers on the display's D-Bus")
            elif frame is None:
                # On X11 the dial skips the taskbar, and the runner leaves such windows out.
                print("skip  KWin's frame of the dial: KWin's window runner does not list it")
            else:
                # A title bar adds a few dozen pixels; the margin covers rounding at a fractional scale.
                check("KWin draws no title bar on the dial", frame[2] <= opened["height"] + 2,
                      f"KWin's frame {frame}, the dial {opened['width']}x{opened['height']}")
        client.call("keyboard_press", keys="Escape", target=opened["ref"])
        check(f"{label}: Escape closes the Speed Dial",
              reached(lambda: speed_dial(client) is None, "the Speed Dial is gone"))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(f"{'ok  ' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"\n      {detail}"))
    for name, why in skipped:
        print(f"SKIP  {name}: {why}")
    print(f"{len(checks) - len(failed)} of {len(checks)} checks passed" + (f", {len(skipped)} skipped" if skipped else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
