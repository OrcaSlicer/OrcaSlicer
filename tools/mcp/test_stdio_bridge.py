#!/usr/bin/env python3
"""Checks `orca-slicer --mcp-stdio` against an instance started with `orca_mcp.py launch`.

    tools/mcp/orca_mcp.py launch --headless
    tools/mcp/test_stdio_bridge.py [--bin build/src/Release/orca-slicer]

The bridge finds the instance through the discovery file in the launch's datadir, so this also
checks that file. Every line a bridge writes is kept, in order: each request gets exactly one reply
line, a malformed line gets one error line with no id, and a notification gets none. A second
bridge sends the instance a 2026-07-28 request as its only line, with no handshake. A third, given
an empty datadir and a port nothing serves, must answer the handshake and list the same tools
without starting OrcaSlicer. A fourth, on a fresh datadir, must start OrcaSlicer for its first tool
call, serving on the bridge's port; the test stops that app. Exits 0 when every check passes.
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def read_discovery(datadir):
    try:
        with open(os.path.join(datadir, "mcp", "server.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def stop_process(pid):
    """Ends an app the bridge started, which outlives the bridge and is no child of this test, and
    waits up to 10 s for it to go. Returns whether it is gone."""
    if os.name == "nt":
        # TerminateProcess only starts the end; waiting on the handle sees it through.
        import ctypes
        from ctypes import wintypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = wintypes.HANDLE
        kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        synchronize, terminate = 0x00100000, 0x0001
        handle = kernel32.OpenProcess(synchronize | terminate, False, pid)
        if not handle:
            return ctypes.get_last_error() == 87  # ERROR_INVALID_PARAMETER: no such process
        try:
            kernel32.TerminateProcess(handle, 1)
            return kernel32.WaitForSingleObject(handle, 10000) == 0  # WAIT_OBJECT_0
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return True
    for _ in range(100):
        try:
            os.kill(pid, 0)
        except OSError:
            return True
        time.sleep(0.1)
    return False


def bridge(binary, datadir, lines, env, extra=()):
    """Feeds `lines` to one bridge process and returns every line it wrote, in order, each parsed as one reply (a
    line that is not JSON as {"line": its text})."""
    proc = subprocess.run([binary, "--mcp-stdio", "--datadir", datadir, *extra], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, timeout=120, env=env)
    replies = []
    for line in proc.stdout.splitlines():
        try:
            replies.append(json.loads(line))
        except ValueError:
            replies.append({"line": line})
    return replies


def ids(replies):
    return [r.get("id") for r in replies]


def reply(replies, id):
    """The first reply with this id, or {}."""
    return next((r for r in replies if r.get("id") == id), {})


def app_state_of(msg):
    """The app_state a tools/call reply's text content holds, or {}."""
    text = (msg.get("result", {}).get("content") or [{}])[0].get("text", "")
    return json.loads(text) if text.startswith("{") else {}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--bin", help="orca-slicer binary (default: the one launch found)")
    args = parser.parse_args()

    _, session = orca_mcp.session_client()
    binary = orca_mcp.find_binary(args.bin)
    # The bridge must find the token on its own, in the discovery file.
    env = {k: v for k, v in os.environ.items() if k != "ORCA_MCP_TOKEN"}
    env.setdefault("DISPLAY", session.get("display") or "")

    legacy = "2025-06-18"
    modern = "2026-07-28"

    def modern_app_state(id):
        return json.dumps({"jsonrpc": "2.0", "id": id, "method": "tools/call",
                           "params": {"name": "app_state", "arguments": {},
                                      "_meta": {"io.modelcontextprotocol/protocolVersion": modern}}})

    replies = bridge(binary, session["datadir"], [
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": legacy, "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}),
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
        json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
        json.dumps({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "app_state", "arguments": {}}}),
        "{not json",
        modern_app_state(4),
        json.dumps({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "no_such_tool", "arguments": {}}}),
    ], env)

    failures = []

    def check(name, ok, detail=""):
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        if not ok:
            failures.append(name)

    init = reply(replies, 1).get("result", {})
    check("initialize negotiates the client's version", init.get("protocolVersion") == legacy, reply(replies, 1))
    # One line per request and one for the malformed line, in order: a reply to the notification would be a
    # seventh line, or a second with no id.
    check("the notification gets no reply", ids(replies) == [1, 2, 3, None, 4, 5], ids(replies))
    tools = reply(replies, 2).get("result", {}).get("tools", [])
    names = [t["name"] for t in tools]
    check("tools/list lists app_state and widget_list", "app_state" in names and "widget_list" in names, names[:5])
    state = app_state_of(reply(replies, 3))
    check("app_state answers through the bridge", state.get("ready") is True, state)
    check("malformed input is a parse error", reply(replies, None).get("error", {}).get("code") == -32700,
          reply(replies, None))
    check("a 2026-07-28 request after a 2025-06-18 handshake is served",
          reply(replies, 4).get("result", {}).get("resultType") == "complete", reply(replies, 4))
    check("an unknown tool is a JSON-RPC error", reply(replies, 5).get("error", {}).get("code") == -32602,
          reply(replies, 5))

    # A 2026-07-28 request needs no handshake: a bridge of its own gets it as its only line.
    alone = bridge(binary, session["datadir"], [modern_app_state(1)], env)
    first = alone[0] if alone else {}
    check("a 2026-07-28 request is served without a handshake",
          ids(alone) == [1] and first.get("result", {}).get("resultType") == "complete" and
          app_state_of(first).get("ready") is True, alone)

    # With no instance answering, everything but a tool call is answered from the bridge's own catalog.
    empty = tempfile.mkdtemp(prefix="orca-bridge-offline-")
    try:
        offline = bridge(binary, empty, [
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"protocolVersion": legacy, "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}),
            json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
            json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
            json.dumps({"jsonrpc": "2.0", "id": 3, "method": "ping"}),
            json.dumps({"jsonrpc": "2.0", "id": 4, "method": "resources/list"}),
            json.dumps({"jsonrpc": 2, "id": 5, "method": "ping"}),
            json.dumps({"jsonrpc": "2.0", "id": 6, "method": "ping"}),
        ], env, ["--mcp-port", str(free_port())])
        check("offline: initialize negotiates the client's version",
              reply(offline, 1).get("result", {}).get("protocolVersion") == legacy, reply(offline, 1))
        check("offline: the notification gets no reply", ids(offline) == [1, 2, 3, 4, None, 6], ids(offline))
        offline_tools = reply(offline, 2).get("result", {}).get("tools")
        check("offline: tools/list matches the running app's", offline_tools == tools, len(offline_tools or []))
        check("offline: ping answers", reply(offline, 3).get("result") == {}, reply(offline, 3))
        check("offline: an unknown method is the server's error",
              reply(offline, 4).get("error", {}).get("code") == -32601, reply(offline, 4))
        check("offline: a jsonrpc that is not a string is an invalid request",
              reply(offline, None).get("error", {}).get("code") == -32600, reply(offline, None))
        check("offline: the bridge answers after a malformed message", reply(offline, 6).get("result") == {},
              reply(offline, 6))
        check("offline: OrcaSlicer was not started", os.listdir(empty) == [], os.listdir(empty))
    finally:
        shutil.rmtree(empty, ignore_errors=True)

    # With nothing answering, a tool call starts OrcaSlicer, which serves on the bridge's port.
    fresh = tempfile.mkdtemp(prefix="orca-bridge-launch-")
    port = free_port()
    orca_mcp.seed_datadir(fresh, orca_mcp.DEFAULT_PRINTER)
    # The app the bridge starts gets what an instance from launch gets: the session's private display
    # when it has one, and a WebView2 profile in its own datadir.
    launch_env = {k: v for k, v in env.items() if k != "WEBVIEW2_USER_DATA_FOLDER"}
    orca_mcp.app_env(launch_env, fresh, session.get("display_env"))
    try:
        launched = bridge(binary, fresh, [
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"protocolVersion": legacy, "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}),
            json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "app_state", "arguments": {}}}),
        ], launch_env, ["--mcp-port", str(port)])
        state = app_state_of(reply(launched, 2))
        # Not ready: a dialog the fresh datadir opens at startup may still be up.
        check("launch: a tool call is answered by the OrcaSlicer it started, on the bridge's port",
              state.get("mcp", {}).get("port") == port, reply(launched, 2))
        discovery = read_discovery(fresh)
        check("launch: the fresh datadir's discovery file names the bridge's port", discovery.get("port") == port, discovery)
    finally:
        pid = read_discovery(fresh).get("pid")
        if pid:
            check("launch: the started app stops", stop_process(pid), pid)
        shutil.rmtree(fresh, ignore_errors=True)

    print("passed" if not failures else f"{len(failures)} failed")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
