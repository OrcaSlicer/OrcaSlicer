#!/usr/bin/env python3
"""Zero-dependency stdio MCP server bridging to the SnapOrca/Orca-CAD control socket.

Speaks MCP (JSON-RPC 2.0 over newline-delimited stdio) to an MCP client (Claude Code),
and forwards each tool call to the app's Unix-domain control socket (opened by the GUI
when launched with SNAPORCA_MCP set). The tool list is built *live* from the app's own
`describe_tools` reply — introspection drives the schema, so new kernel methods surface
without touching this file.

Usage:  snaporca_mcp_bridge.py [SOCKET_PATH]   (default /tmp/snaporca-mcp.sock)
The app must be running with SNAPORCA_MCP set; if the socket is down, tools/list falls
back to the slice-1 set and tool calls report the connection error (never crash).
"""
import sys, os, json, socket, itertools

SOCK_PATH = sys.argv[1] if len(sys.argv) > 1 else "/tmp/snaporca-mcp.sock"
SERVER_INFO = {"name": "snaporca-cad", "version": "0.1"}
_app_id = itertools.count(1)

# --- app control-socket round-trip --------------------------------------------
def app_call(method, params=None):
    """One request to the app over the unix socket. Raises on transport failure."""
    req = {"jsonrpc": "2.0", "id": next(_app_id), "method": method}
    if params is not None:
        req["params"] = params
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(20)
    try:
        s.connect(SOCK_PATH)
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()
    return json.loads(buf.decode())

# --- describe_tools -> MCP tool schemas ---------------------------------------
_TYPE_MAP = {"number": "number", "integer": "integer", "string": "string", "boolean": "boolean"}

def _param_schema(p):
    sch = {"type": _TYPE_MAP.get(p.get("type", "string"), "string")}
    if "unit" in p:    sch["description"] = f"in {p['unit']}"
    if "enum" in p:    sch["enum"] = p["enum"]
    if "min" in p:     sch["minimum"] = p["min"]
    if "max" in p:     sch["maximum"] = p["max"]
    if "default" in p: sch["default"] = p["default"]
    return sch

# Slice-1 fallback when the app socket is unreachable at list time.
_FALLBACK_TOOLS = {"tools": [
    {"name": "describe_tools", "summary": "List callable tools and their parameters.", "params": []},
    {"name": "describe_scene", "summary": "Feature tree + per-body bounding boxes.", "params": []},
    {"name": "extrude", "summary": "Create a solid: a centred rectangle sketch extruded to a depth.",
     "params": [{"name": "width", "type": "number", "unit": "mm", "default": 20},
                {"name": "height", "type": "number", "unit": "mm", "default": 20},
                {"name": "distance", "type": "number", "unit": "mm", "default": 10},
                {"name": "plane", "type": "string", "enum": ["XY", "XZ", "YZ"], "default": "XY"}]},
]}

def list_tools():
    try:
        desc = app_call("describe_tools").get("result", {})
        if "tools" not in desc:
            desc = _FALLBACK_TOOLS
    except Exception:
        desc = _FALLBACK_TOOLS
    out = []
    for t in desc["tools"]:
        params = t.get("params", [])
        props = {p["name"]: _param_schema(p) for p in params}
        required = [p["name"] for p in params if "default" not in p]
        out.append({
            "name": t["name"],
            "description": t.get("summary", ""),
            "inputSchema": {"type": "object", "properties": props, "required": required},
        })
    return out

# --- MCP method handlers ------------------------------------------------------
def handle(req):
    m = req.get("method")
    rid = req.get("id")
    if m == "initialize":
        ver = (req.get("params") or {}).get("protocolVersion", "2024-11-05")
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": ver,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": SERVER_INFO}}
    if m == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}
    if m == "tools/list":
        return {"jsonrpc": "2.0", "id": rid, "result": {"tools": list_tools()}}
    if m == "tools/call":
        p = req.get("params") or {}
        name = p.get("name")
        args = p.get("arguments") or {}
        try:
            reply = app_call(name, args)
        except Exception as e:
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": f"control socket unreachable ({SOCK_PATH}): {e}"}],
                "isError": True}}
        if "error" in reply:
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": json.dumps(reply["error"])}], "isError": True}}
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "content": [{"type": "text", "text": json.dumps(reply.get("result"), indent=2)}]}}
    if rid is not None:  # unknown *request*
        return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": f"method not found: {m}"}}
    return None  # notification (e.g. notifications/initialized) -> no reply

def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        resp = handle(req)
        if resp is not None:
            sys.stdout.write(json.dumps(resp) + "\n")
            sys.stdout.flush()

if __name__ == "__main__":
    main()
