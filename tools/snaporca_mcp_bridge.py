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

# Fallback when the app socket is unreachable at list time (e.g. the GUI isn't up yet when the
# MCP client lists tools at session start). The LIVE describe_tools reply is authoritative — this
# mirrors its 16-method surface so the agent still sees the full toolset; params without a
# `default` are surfaced as required. Keep in sync with McpControl.cpp describe_tools().
def _p(name, typ="number", **kw): return dict(name=name, type=typ, **kw)
_PLANE = _p("plane", "string", enum=["XY", "XZ", "YZ"], default="XY")
_FALLBACK_TOOLS = {"tools": [
    {"name": "describe_tools", "summary": "List callable tools and their parameters.", "params": []},
    {"name": "describe_scene", "summary": "Feature tree + per-body bounding boxes.", "params": []},
    {"name": "extrude", "summary": "Extrude a profile (or width x height rectangle) to a depth; Onshape end conditions.",
     "params": [_p("width", default=20), _p("height", default=20), _p("distance", default=10), _PLANE,
                _p("profile", "array", default=[], description="closed [[x,y],...] overrides width/height"),
                _p("boolean", "string", enum=["new", "union", "subtract", "intersect"], default="new"),
                _p("end", "string", enum=["blind", "symmetric", "two_sided", "through_all", "up_to_face"], default="blind"),
                _p("distance2", default=0, description="second side when end=two_sided"),
                _p("up_to_face", "integer", default=-1, description="target face id when end=up_to_face"),
                _p("taper", default=0), _p("flip", "boolean", default=False)]},
    {"name": "revolve", "summary": "Revolve a profile about a plane axis.",
     "params": [_p("width", default=20), _p("height", default=10), _p("angle", default=360),
                _p("axis", "integer", enum=[0, 1], default=0), _p("flip", "boolean", default=False), _PLANE,
                _p("profile", "array", default=[], description="closed [[x,y],...] overrides width/height"),
                _p("boolean", "string", enum=["new", "union", "subtract", "intersect"], default="new")]},
    {"name": "fillet", "summary": "Round a measured edge of a body.",
     "params": [_p("edge", "integer"), _p("radius", default=1),
                _p("body", "integer", default=-1, description="target body; omit for last")]},
    {"name": "chamfer", "summary": "Chamfer a measured edge of a body.",
     "params": [_p("edge", "integer"), _p("distance", default=1),
                _p("body", "integer", default=-1, description="target body; omit for last")]},
    {"name": "hole", "summary": "Drill a circular hole at (x,y) on a plane.",
     "params": [_p("diameter", default=5), _p("depth", default=10), _p("through", "boolean", default=False),
                _p("x", default=0), _p("y", default=0), _PLANE]},
    {"name": "boolean", "summary": "Combine two bodies: union | subtract | intersect.",
     "params": [_p("op", "string", enum=["union", "subtract", "intersect"], default="subtract"),
                _p("target", "integer", default=0), _p("tool", "integer", default=1),
                _p("keep_tool", "boolean", default=False), _p("tolerance", default=0)]},
    {"name": "pattern", "summary": "Replicate a body: linear or circular.",
     "params": [_p("circular", "boolean", default=False), _p("count", "integer", default=3),
                _p("spacing", default=10), _p("dir", "integer", enum=[0, 1], default=0),
                _p("angle", default=360), _PLANE, _p("body", "integer", default=-1, description="target body; omit for last")]},
    {"name": "shell", "summary": "Hollow a body to a wall thickness; optionally open one face.",
     "params": [_p("thickness", default=1), _p("face", "integer", default=-1, description="face id to leave open; omit for closed"),
                _p("body", "integer", default=-1, description="target body; omit for last")]},
    {"name": "draft", "summary": "Taper a body face by an angle (pull +Z).",
     "params": [_p("face", "integer"), _p("angle", default=5),
                _p("body", "integer", default=-1, description="target body; omit for last")]},
    {"name": "query_topology", "summary": "Measured faces and edges of a body.",
     "params": [_p("body", "integer", default=0)]},
    {"name": "measure", "summary": "Distance/angle between two refs {face|edge|point} on a body.",
     "params": [_p("body", "integer", default=0), _p("a", "object"), _p("b", "object")]},
    {"name": "slice_body", "summary": "Cross-section of a body; ordered closed/open contours.",
     "params": [_p("body", "integer", default=0), _PLANE, _p("offset", default=0)]},
    {"name": "import_step", "summary": "Import a STEP file as native B-rep bodies.",
     "params": [_p("path", "string")]},
    {"name": "validate_against", "summary": "Volume + bbox + surface deviation of a body vs a reference {step|body}.",
     "params": [_p("body", "integer", default=0), _p("reference", "object")]},
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
