#!/usr/bin/env python3
"""Checks `orca-slicer --mcp-stdio` against an instance started with `orca_mcp.py launch`.

    tools/mcp/orca_mcp.py launch --xvfb
    tools/mcp/test_stdio_bridge.py [--bin build/src/Release/orca-slicer]

The bridge finds the instance through the discovery file in the launch's datadir, so this also
checks that file. A second bridge, given an empty datadir and a port nothing serves, must answer the
handshake and list the same tools without starting OrcaSlicer. A third, on a fresh datadir, must
start OrcaSlicer for its first tool call, serving on the bridge's port; the test stops that app.
Exits 0 when every check passes.
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
    """Feeds `lines` to one bridge process and returns its replies by id."""
    proc = subprocess.run([binary, "--mcp-stdio", "--datadir", datadir, *extra], input="\n".join(lines) + "\n",
                          capture_output=True, text=True, timeout=120, env=env)
    replies = {}
    for line in proc.stdout.splitlines():
        msg = json.loads(line)
        replies[msg.get("id")] = msg
    return replies


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
    replies = bridge(binary, session["datadir"], [
        json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"protocolVersion": legacy, "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}),
        json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}),
        json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/list"}),
        json.dumps({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "get_app_state", "arguments": {}}}),
        "{not json",
        json.dumps({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "get_app_state", "arguments": {},
                               "_meta": {"io.modelcontextprotocol/protocolVersion": modern}}}),
        json.dumps({"jsonrpc": "2.0", "id": 5, "method": "tools/call", "params": {"name": "no_such_tool", "arguments": {}}}),
    ], env)

    failures = []

    def check(name, ok, detail=""):
        print(("ok   " if ok else "FAIL ") + name + ("" if ok else f": {detail}"))
        if not ok:
            failures.append(name)

    init = replies.get(1, {}).get("result", {})
    check("initialize negotiates the client's version", init.get("protocolVersion") == legacy, replies.get(1))
    check("the notification gets no reply", set(replies) <= {1, 2, 3, 4, 5, None}, sorted(map(str, replies)))
    names = [t["name"] for t in replies.get(2, {}).get("result", {}).get("tools", [])]
    check("tools/list lists get_app_state and list_widgets", "get_app_state" in names and "list_widgets" in names, names[:5])
    state = json.loads(replies.get(3, {}).get("result", {}).get("content", [{}])[0].get("text", "{}"))
    check("get_app_state answers through the bridge", state.get("ready") is True, state)
    check("malformed input is a parse error", replies.get(None, {}).get("error", {}).get("code") == -32700, replies.get(None))
    modern_result = replies.get(4, {}).get("result", {})
    check("a 2026-07-28 request is served without a handshake", modern_result.get("resultType") == "complete", replies.get(4))
    check("an unknown tool is a JSON-RPC error", replies.get(5, {}).get("error", {}).get("code") == -32602, replies.get(5))

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
              offline.get(1, {}).get("result", {}).get("protocolVersion") == legacy, offline.get(1))
        check("offline: the notification gets no reply", set(offline) <= {1, 2, 3, 4, 6, None}, sorted(map(str, offline)))
        check("offline: tools/list matches the running app's",
              offline.get(2, {}).get("result", {}).get("tools") == replies.get(2, {}).get("result", {}).get("tools"),
              len(offline.get(2, {}).get("result", {}).get("tools", [])))
        check("offline: ping answers", offline.get(3, {}).get("result") == {}, offline.get(3))
        check("offline: an unknown method is the server's error", offline.get(4, {}).get("error", {}).get("code") == -32601, offline.get(4))
        check("offline: a jsonrpc that is not a string is an invalid request",
              offline.get(None, {}).get("error", {}).get("code") == -32600, offline.get(None))
        check("offline: the bridge answers after a malformed message", offline.get(6, {}).get("result") == {}, offline.get(6))
        check("offline: OrcaSlicer was not started", os.listdir(empty) == [], os.listdir(empty))
    finally:
        shutil.rmtree(empty, ignore_errors=True)

    # With nothing answering, a tool call starts OrcaSlicer, which serves on the bridge's port.
    fresh = tempfile.mkdtemp(prefix="orca-bridge-launch-")
    port = free_port()
    orca_mcp.seed_datadir(fresh, orca_mcp.DEFAULT_PRINTER)
    # The app the bridge starts gets what an instance from launch gets: the session's Xvfb display
    # when it has one, and a WebView2 profile in its own datadir.
    launch_env = {k: v for k, v in env.items() if k != "WEBVIEW2_USER_DATA_FOLDER"}
    orca_mcp.app_env(launch_env, fresh, session.get("display") if session.get("xvfb_pid") else None)
    try:
        launched = bridge(binary, fresh, [
            json.dumps({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"protocolVersion": legacy, "capabilities": {}, "clientInfo": {"name": "test", "version": "1"}}}),
            json.dumps({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": "get_app_state", "arguments": {}}}),
        ], launch_env, ["--mcp-port", str(port)])
        text = launched.get(2, {}).get("result", {}).get("content", [{}])[0].get("text", "")
        state = json.loads(text) if text.startswith("{") else {}
        # Not ready: a dialog the fresh datadir opens at startup may still be up.
        check("launch: a tool call is answered by the OrcaSlicer it started, on the bridge's port",
              state.get("mcp", {}).get("port") == port, launched.get(2))
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
