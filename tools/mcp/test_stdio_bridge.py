#!/usr/bin/env python3
"""Checks `orca-slicer --mcp-stdio` against an instance started with `orca_mcp.py launch`.

    tools/mcp/orca_mcp.py launch --xvfb
    tools/mcp/test_stdio_bridge.py [--bin build/src/Release/orca-slicer]

The bridge finds the instance through the discovery file in the launch's datadir, so this also
checks that file. Exits 0 when every check passes.
"""

import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def bridge(binary, datadir, lines, env):
    """Feeds `lines` to one bridge process and returns its replies by id."""
    proc = subprocess.run([binary, "--mcp-stdio", "--datadir", datadir], input="\n".join(lines) + "\n",
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
        json.dumps({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": "get_state", "arguments": {}}}),
        "{not json",
        json.dumps({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                    "params": {"name": "get_state", "arguments": {},
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
    check("tools/list lists get_state and ui_snapshot", "get_state" in names and "ui_snapshot" in names, names[:5])
    state = json.loads(replies.get(3, {}).get("result", {}).get("content", [{}])[0].get("text", "{}"))
    check("get_state answers through the bridge", state.get("ready") is True, state)
    check("malformed input is a parse error", replies.get(None, {}).get("error", {}).get("code") == -32700, replies.get(None))
    modern_result = replies.get(4, {}).get("result", {})
    check("a 2026-07-28 request is served without a handshake", modern_result.get("resultType") == "complete", replies.get(4))
    check("an unknown tool is a JSON-RPC error", replies.get(5, {}).get("error", {}).get("code") == -32602, replies.get(5))

    print("passed" if not failures else f"{len(failures)} failed")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
