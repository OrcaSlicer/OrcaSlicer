#!/usr/bin/env python3
"""Checks how orca_mcp.py attaches to a running OrcaSlicer and calls CAD tools, without the app.

    python tools/mcp/test_orca_mcp_attach.py

default_datadir() takes a portable data_dir beside the binary (beside the .app on macOS) and
otherwise the platform's configuration folder. from_datadir() refuses a datadir without
mcp/server.json and one whose server.json names a port where nothing listens. call_cad_tool(),
against a stubbed call(), fails at once on a dialog, follows a running job, prefixes its errors with
the CAD tool's name and returns the tool's result. Exits 0 when every check passes.
"""

import json
import os
import socket
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from orca_mcp import McpError, OrcaMcp  # noqa: E402


def ok(name):
    print(f"ok - {name}")


def same_path(a, b):
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


def raises(fn, *needles):
    """Runs fn, which must raise McpError whose message contains every needle; returns the message."""
    try:
        fn()
    except McpError as e:
        message = str(e)
        for needle in needles:
            assert needle in message, f"{needle!r} not in {message!r}"
        return message
    raise AssertionError("no McpError raised")


def binary_in(folder):
    """Where the OrcaSlicer binary of an install in `folder` lives."""
    if sys.platform == "darwin":
        return os.path.join(folder, "OrcaSlicer.app", "Contents", "MacOS", "OrcaSlicer")
    return os.path.join(folder, "orca-slicer.exe" if os.name == "nt" else "orca-slicer")


def check_default_datadir():
    install = tempfile.mkdtemp(prefix="orca-attach-portable-")
    os.makedirs(os.path.join(install, "data_dir"))
    assert same_path(orca_mcp.default_datadir(binary_in(install)), os.path.join(install, "data_dir"))
    ok("default_datadir() takes the portable data_dir beside the binary")

    home = tempfile.mkdtemp(prefix="orca-attach-home-")
    no_portable = binary_in(tempfile.mkdtemp(prefix="orca-attach-installed-"))
    if sys.platform == "win32":
        cases = [({"APPDATA": home}, os.path.join(home, "OrcaSlicer"))]
    elif sys.platform == "darwin":
        cases = [({"HOME": home}, os.path.join(home, "Library", "Application Support", "OrcaSlicer"))]
    else:
        xdg = os.path.join(home, "xdg")
        cases = [({"XDG_CONFIG_HOME": xdg, "HOME": home}, os.path.join(xdg, "OrcaSlicer")),
                 ({"XDG_CONFIG_HOME": "", "HOME": home}, os.path.join(home, ".config", "OrcaSlicer"))]
    for env, expected in cases:
        with mock.patch.dict(os.environ, env):
            for exe in (None, no_portable):
                got = orca_mcp.default_datadir(exe)
                assert same_path(got, expected), f"{sys.platform} {env} exe={exe}: {got} != {expected}"
    ok(f"default_datadir() falls back to the {sys.platform} configuration folder")


def check_from_datadir():
    empty = tempfile.mkdtemp(prefix="orca-attach-empty-")
    raises(lambda: OrcaMcp.from_datadir(empty), os.path.join(empty, "mcp", "server.json"), "--mcp")
    ok("from_datadir() without server.json names the file and says to start with --mcp")

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    stale = tempfile.mkdtemp(prefix="orca-attach-stale-")
    os.makedirs(os.path.join(stale, "mcp"))
    with open(os.path.join(stale, "mcp", "server.json"), "w") as f:
        json.dump({"url": f"http://127.0.0.1:{port}/mcp", "port": port, "pid": 0, "version": "0", "token": "t"}, f)
    raises(lambda: OrcaMcp.from_datadir(stale), str(port), "--mcp")
    ok("from_datadir() with a stale server.json says nothing answers on its port")


def stub(client, *replies):
    """Replaces client.call with one that hands out `replies` in turn (raising an exception reply)
    and returns the list of calls it saw."""
    calls, queue = [], list(replies)

    def call(tool, /, _timeout=None, **args):
        calls.append((tool, _timeout, args))
        reply = queue.pop(0)
        if isinstance(reply, Exception):
            raise reply
        return reply

    client.call = call
    return calls


def check_call_cad_tool():
    client = OrcaMcp(1, "t")
    calls = stub(client, {"status": "blocked_by_dialog", "job": "j", "dialogs": [{"title": "Save?"}]})
    message = raises(lambda: client.call_cad_tool("extrude", distance=5), "Save?")
    assert message.startswith("extrude: "), message
    assert len(calls) == 1, calls
    ok("call_cad_tool() fails at once on a dialog, naming it")

    calls = stub(client, {"result": {"ok": True}})
    assert client.call_cad_tool("extrude", distance=5) == {"ok": True}
    assert calls == [("call_cad_tool", 600, {"tool": "extrude", "args": {"distance": 5}})], calls
    ok("call_cad_tool() returns the CAD tool's result")

    calls = stub(client, {"status": "running", "job": "j"}, {"result": 7})
    assert client.call_cad_tool("sweep", sketch=1, path=0) == 7
    assert calls[1][0] == "wait" and calls[1][2]["job"] == "j", calls
    ok("call_cad_tool() follows a running job to its result")

    stub(client, McpError("kernel refused the hole"))
    message = raises(lambda: client.call_cad_tool("hole"), "kernel refused the hole")
    assert message.startswith("hole: "), message
    stub(client, {"status": "running", "job": "j"}, McpError("the job failed"))
    message = raises(lambda: client.call_cad_tool("hole"), "the job failed")
    assert message.startswith("hole: "), message
    ok("call_cad_tool() starts every error with the CAD tool's name")


def main():
    check_default_datadir()
    check_from_datadir()
    check_call_cad_tool()
    return 0


if __name__ == "__main__":
    sys.exit(main())
