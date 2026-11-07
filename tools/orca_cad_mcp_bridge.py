#!/usr/bin/env python3
"""Zero-dependency stdio MCP server bridging to the Orca/Orca-CAD control socket.

Speaks MCP (JSON-RPC 2.0 over newline-delimited stdio) to an MCP client (Claude Code),
and forwards each tool call to the app's Unix-domain control socket (opened by the GUI
when launched with ORCA_CAD_MCP set). The tool list is built *live* from the app's own
`describe_tools` reply — introspection drives the schema, so new kernel methods surface
without touching this file.

Usage:  orca_cad_mcp_bridge.py [SOCKET_PATH]   (default /tmp/orca-cad-mcp.sock)
The app must be running with ORCA_CAD_MCP set; if the socket is down, tools/list serves the last
cached describe_tools reply and tool calls report the connection error (never crash).
"""
import sys, os, json, socket, itertools

SOCK_PATH = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
SERVER_INFO = {"name": "orca-cad", "version": "0.1"}
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
_TYPE_MAP = {"number": "number", "integer": "integer", "string": "string",
             "boolean": "boolean", "array": "array", "object": "object"}

def _param_schema(p):
    sch = {"type": _TYPE_MAP.get(p.get("type", "string"), "string")}
    desc = p.get("description")
    if "unit" in p:    desc = (desc + " " if desc else "") + f"in {p['unit']}"
    if desc:           sch["description"] = desc
    if "enum" in p:    sch["enum"] = p["enum"]
    if "min" in p:     sch["minimum"] = p["min"]
    if "max" in p:     sch["maximum"] = p["max"]
    if "default" in p: sch["default"] = p["default"]
    return sch

# The app's describe_tools reply is the ONLY source of the tool list (no hand-kept copy to drift). The last good
# reply is cached, so a client that lists tools before the GUI is up still sees the whole surface.
CACHE = os.path.expanduser("~/.cache/orca_cad_mcp_tools.json")

def _describe():
    try:
        desc = app_call("describe_tools").get("result", {})
        if "tools" in desc:
            try:
                os.makedirs(os.path.dirname(CACHE), exist_ok=True)
                with open(CACHE, "w") as f: json.dump(desc, f)
            except OSError:
                pass
            return desc
    except Exception:
        pass
    try:
        with open(CACHE) as f: return json.load(f)
    except (OSError, ValueError):
        return {"tools": []}

def list_tools():
    out = []
    for t in _describe()["tools"]:
        params = t.get("params", [])
        props = {p["name"]: _param_schema(p) for p in params}
        required = [p["name"] for p in params if "default" not in p]
        out.append({
            "name": t["name"],
            "description": t.get("summary", ""),
            "inputSchema": {"type": "object", "properties": props, "required": required},
            "annotations": {"readOnlyHint": bool(t.get("readOnly")), "destructiveHint": bool(t.get("destructive"))},
        })
    return out

# --- MCP method handlers ------------------------------------------------------
def handle(req):
    m = req.get("method")
    rid = req.get("id")
    if m == "initialize":
        want = (req.get("params") or {}).get("protocolVersion")
        ver = want if want in ("2025-06-18", "2025-03-26", "2024-11-05") else "2025-06-18"
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": ver,
            "capabilities": {"tools": {"listChanged": False}, "resources": {}},
            "serverInfo": SERVER_INFO}}
    if m == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}
    if m == "resources/list":
        return {"jsonrpc": "2.0", "id": rid, "result": {"resources": [
            {"uri": "orca-cad://design", "name": "design", "mimeType": "application/json",
             "description": "Live state of the open design: parameters, features with errors, bodies with volume/COM/bbox, sketches, undo depth (inspect_design)."},
            {"uri": "orca-cad://tools", "name": "tools", "mimeType": "application/json",
             "description": "Every callable method with its parameters (describe_tools)."}]}}
    if m == "resources/read":
        uri = (req.get("params") or {}).get("uri")
        method = {"orca-cad://design": "inspect_design", "orca-cad://tools": "describe_tools"}.get(uri)
        if method is None:
            return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32602, "message": f"unknown resource: {uri}"}}
        try:
            body = app_call(method).get("result")
        except Exception as e:
            return {"jsonrpc": "2.0", "id": rid, "error": {"code": -32603, "message": f"control socket unreachable ({SOCK_PATH}): {e}"}}
        return {"jsonrpc": "2.0", "id": rid, "result": {"contents": [{"uri": uri, "mimeType": "application/json", "text": json.dumps(body, indent=2)}]}}
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
        res = reply.get("result")
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "content": [{"type": "text", "text": json.dumps(res, indent=2)}],
            "isError": isinstance(res, dict) and res.get("ok") is False}}
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
