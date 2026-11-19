"""Checks that view_set refuses sidebar_width while the sidebar floats in a window of its own.

No command floats the sidebar; a person drags its caption off the dock, and the window layout saved keeps it floating.
So this test launches OrcaSlicer itself, twice, on a fresh data folder. The first run writes its window layout
(OrcaSlicer.conf, app.window_layout) as it starts its empty project (Plater::priv::reset()), and the app writes
OrcaSlicer.conf at its next idle moment; orca_mcp.py stop then ends it, on Windows by killing the process, which saves
nothing more. The test marks the sidebar's entry of that layout floating, as the drag leaves it, and the second run
starts with the sidebar floating. There view_set {sidebar_width} is refused and the width stays as it was. A native
Wayland app drops the saved floating state, so on Linux both runs start as X11 clients (GDK_BACKEND=x11, through
XWayland on a Wayland desktop), as the sweep's own launches do by default, and the checks run on every platform. Each
run is stopped on every path out, and nothing but a run this test started is stopped.
Run it as python tools/mcp/test_sidebar_floating.py [--bin PATH] [--datadir PATH].
"""
import contextlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from orca_mcp import McpError, OrcaMcp, app_env  # noqa: E402

TOKEN = "sidebar-floating-test-token"
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)[:400]), flush=True)


def free_port():
    """A port nothing listens on now, so that this test runs beside the others of a sweep."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def harness(env, *args):
    return subprocess.run([sys.executable, os.path.join(HERE, "orca_mcp.py"), *args], env=env, capture_output=True, text=True,
                          timeout=300)


@contextlib.contextmanager
def launched(datadir, binary):
    """An instance on `datadir`: its client, or None when it did not start, stopped on every path out of the block.

    A launch writes its session file before it waits for the app, and one that is not ready exits leaving its app
    running, so the stop goes by that file whenever it is there. Only this launch writes it: a file an earlier run left
    is removed first, so that no other app is ever stopped."""
    port = free_port()
    session = os.path.join(datadir, "session.json")
    env = app_env(dict(os.environ, ORCA_MCP_SESSION=session, ORCA_MCP_TOKEN=TOKEN), datadir)
    if sys.platform.startswith("linux"):
        env["GDK_BACKEND"] = "x11"  # a native Wayland app drops the saved floating state
    if os.path.exists(session):
        os.remove(session)
    try:
        started = harness(env, "launch", "--datadir", datadir, "--port", str(port), *binary)
        if started.returncode != 0:
            print(started.stdout, started.stderr)
            yield None
            return
        orca = OrcaMcp(port=port, token=TOKEN)
        orca.call("app_wait", **{"for": "ready"})
        yield orca
    finally:
        if os.path.exists(session):
            harness(env, "stop")
            time.sleep(2)


def float_sidebar(datadir):
    """Marks the sidebar's entry of the saved window layout floating (wxAuiPaneInfo::optionFloating, state bit 0), with a
    place and size of its own window. Returns whether the layout had the entry."""
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    with open(conf, encoding="utf-8") as f:
        text = f.read()
    config = json.loads(text[: text.rfind("}") + 1])  # the JSON, without the checksum line after it
    layout = config.get("app", {}).get("window_layout", "")

    def floating(entry):
        entry = re.sub(r"state=(\d+);", lambda m: f"state={int(m.group(1)) | 1};", entry)
        for key, value in (("floatx", 200), ("floaty", 200), ("floatw", 420), ("floath", 800)):
            entry = re.sub(rf"{key}=-?\d+", f"{key}={value}", entry)
        return entry

    entries = layout.split("|")
    found = any(entry.startswith("name=sidebar;") for entry in entries)
    config["app"]["window_layout"] = "|".join(floating(e) if e.startswith("name=sidebar;") else e for e in entries)
    with open(conf, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=4)
    return found


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="osf-")
    datadir = os.path.join(root, "floating")
    try:
        with launched(datadir, binary) as orca:
            started = orca is not None
        check("the app starts", started)
        if started:
            check("the window layout the first run saved has the sidebar", float_sidebar(datadir))
            with launched(datadir, binary) as orca:
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

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
