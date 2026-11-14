#!/usr/bin/env python3
"""Checks that Space opens the Speed Dial right after a switch to each page with a 3D view, and that KWin draws
no title bar on the dial.

    tools/mcp/orca_mcp.py launch [--cad]
    tools/mcp/test_speed_dial_open.py

Each page is reached from Home by a click on its tab, as a person switches pages: Prepare first, while the
plater still shows the 3D view it started with, then Design when the Design tab is on, and Preview. The page's
3D view must then hold the keyboard focus, as a focused sidebar control keeps Space for itself, and Space sent
to the focused window must open the dial. The view also takes the focus when the pointer enters it, so these
checks prove the switch itself only where no pointer rests on the view: on a native Wayland display, or on a
desktop with the pointer elsewhere. Where KWin answers on the display's D-Bus (the private one of a
`--headless` launch, else the caller's), the frame KWin reports for the dial must be the dial's own size: a
title bar of KWin's makes the frame taller. Run it on a freshly launched app. Exits 0 when every check passes.
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


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

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def wait_for(predicate, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            value = predicate()
            if value:
                return value
            time.sleep(0.25)
        return predicate()

    def tree():
        _, texts = client.call_with_text("list_widgets", max_nodes=3000)
        return "\n".join(texts)

    def switch_to(label, tab):
        """Clicks Home's tab (the first, with no label), then the tab `label`, and waits for get_app_state's tab."""
        tabs = tree()
        home = re.search(r"\[(w\d+)\] tab \[", tabs)
        page = re.search(rf'\[(w\d+)\] tab "{re.escape(label)}"', tabs)
        client.call("click", target=home.group(1))
        wait_for(lambda: client.call("get_app_state")["tab"] == "home", 10)
        client.call("click", target=page.group(1))
        return wait_for(lambda: client.call("get_app_state")["tab"] == tab, 10)

    def dial():
        """The Speed Dial as list_windows gives it: the shown web dialog with no title."""
        return next((w for w in client.call("list_windows")["items"]
                     if w.get("kind") == "dialog" and w.get("web") and not w.get("title")), None)

    pages = [("Prepare", "prepare")]
    if client.call("get_app_state").get("design_tab"):
        pages.append(("Design", "design"))
    pages.append(("Preview", "preview"))

    frame_checked = False
    for label, tab in pages:
        check(f"{label}: the tab is shown", switch_to(label, tab))
        focused = [line.strip() for line in tree().splitlines() if "[focused]" in line]
        check(f"{label}: its 3D view holds the keyboard focus", focused and "canvas (wxGLCanvas)" in focused[0], focused)
        client.call("press_keys", keys="Space")
        opened = wait_for(dial, 10)
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
        client.call("press_keys", keys="Escape", target=opened["ref"])
        check(f"{label}: Escape closes the Speed Dial", wait_for(lambda: dial() is None, 10))

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(f"{'ok  ' if ok else 'FAIL'}  {name}" + ("" if ok or not detail else f"\n      {detail}"))
    print(f"{len(checks) - len(failed)} of {len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
