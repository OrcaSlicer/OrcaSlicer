"""Checks that view_set refuses sidebar_width while the sidebar floats in a window of its own.
Its design is the test_sidebar_floating entry of tools/mcp/test-entries/windows_web.md.

No command floats the sidebar; a person drags its caption off the dock, and the window layout saved keeps it floating.
So this test launches OrcaSlicer itself, twice, on a fresh data folder. The first run writes its window layout
(OrcaSlicer.conf, app.window_layout) as it starts its empty project (Plater::priv::reset()), and the app writes
OrcaSlicer.conf at its next idle moment; once the file holds the layout, orca_mcp.py stop ends the run, on Windows by
killing the process, which saves nothing more. The test marks the sidebar's entry of that layout floating, as the drag
leaves it, and the second run starts with the sidebar floating. There view_set {sidebar_width} is refused and the
width stays as it was. A native Wayland app drops the saved floating state, so on Linux both runs start as X11 clients
(GDK_BACKEND=x11, through XWayland on a Wayland desktop), as the sweep's own launches do by default, and the checks run
on every platform. Each run is stopped on every path out, and nothing but a run this test started is stopped.
Run it as python tools/mcp/test_sidebar_floating.py [--bin PATH] [--datadir PATH].
"""
import json
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import Checks, launched, read_conf, until  # noqa: E402
from orca_mcp import McpError  # noqa: E402

TOKEN = "sidebar-floating-test-token"
# A native Wayland app drops the saved floating state, so on Linux both runs start as X11 clients.
ENV = {"GDK_BACKEND": "x11"} if sys.platform.startswith("linux") else {}
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def layout_saved(datadir):
    """Whether OrcaSlicer.conf holds a window layout with the sidebar's entry; False too while the app writes it."""
    try:
        layout = read_conf(datadir).get("app", {}).get("window_layout", "")
    except (OSError, ValueError):
        return False
    return any(entry.startswith("name=sidebar;") for entry in layout.split("|"))


def float_sidebar(datadir):
    """Marks the sidebar's entry of the saved window layout floating (wxAuiPaneInfo::optionFloating, state bit 0), with a
    place and size of its own window. Returns whether the layout had the entry."""
    config = read_conf(datadir)
    layout = config.get("app", {}).get("window_layout", "")

    def floating(entry):
        entry = re.sub(r"state=(\d+);", lambda m: f"state={int(m.group(1)) | 1};", entry)
        for key, value in (("floatx", 200), ("floaty", 200), ("floatw", 420), ("floath", 800)):
            entry = re.sub(rf"{key}=-?\d+", f"{key}={value}", entry)
        return entry

    entries = layout.split("|")
    found = any(entry.startswith("name=sidebar;") for entry in entries)
    config["app"]["window_layout"] = "|".join(floating(e) if e.startswith("name=sidebar;") else e for e in entries)
    with open(os.path.join(datadir, "OrcaSlicer.conf"), "w", encoding="utf-8") as f:
        json.dump(config, f, indent=4)
    return found


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="osf-")
    datadir = os.path.join(root, "floating")
    try:
        with launched(datadir, binary, TOKEN, env=ENV) as orca:
            started = orca is not None
            # The stop writes nothing (on Windows it kills the app), so the run goes on until the app has written the
            # layout at its next idle moment.
            if started:
                try:
                    until(lambda: layout_saved(datadir), "OrcaSlicer.conf holds the window layout with the sidebar")
                except TimeoutError as e:
                    print(e, flush=True)
        check("the app starts", started)
        if started:
            check("the window layout the first run saved has the sidebar", float_sidebar(datadir))
            with launched(datadir, binary, TOKEN, env=ENV) as orca:
                if orca is None:
                    check("the app starts again", False)
                else:
                    width = orca.call("app_state")["view"]["sidebar_width"]
                    try:
                        reply = orca.call("view_set", sidebar_width=width + 100)
                        check("view_set of sidebar_width while the sidebar floats is refused", False, reply)
                    except McpError as e:
                        check("view_set of sidebar_width while the sidebar floats is refused",
                              "The sidebar floats in a window of its own" in str(e), str(e))
                    after = orca.call("app_state")["view"]["sidebar_width"]
                    check("and the sidebar's width stays as it was", after == width, (width, after))
    finally:
        if not given:
            shutil.rmtree(root, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
