#!/usr/bin/env python3
"""The agent-facing vocabulary: list_commands (tools + ribbon verbs, with the reason a verb cannot run), execute (a tool or a verb by
name), set_parameter (rebuild counts) and export in every format. Runs against a RUNNING app.

    python3 scripts/CAD/check-mcp-commands.py [socket]
"""
import json, os, socket, sys

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_n = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(90)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": _n, "method": method, "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    r = json.loads(buf.decode())
    if "error" in r:
        raise RuntimeError("%s: %s" % (method, r["error"]))
    return r["result"]


fails = []


def check(cond, msg):
    print(("ok   " if cond else "FAIL ") + msg)
    if not cond:
        fails.append(msg)


call("new_document")
cmds = call("list_commands")["commands"]
names = {c["command"]: c for c in cmds}   # tools by name, ribbon verbs as "verb:<id>"
check("extrude" in names and names["extrude"]["kind"] == "tool", "list_commands lists the extrude tool")
check("verb:sheet_flange" in names and names["verb:sheet_flange"]["kind"] == "verb", "list_commands lists the sheet flange verb")
check(names["verb:sheet_flange"]["enabled"] is False and names["verb:sheet_flange"].get("reason"), "a verb that cannot run says why: %r" % names["verb:sheet_flange"].get("reason"))
check(all(c["command"] != "execute" for c in cmds), "execute does not list itself")
check(len(call("list_commands", filter="sheet")["commands"]) >= 7, "filter narrows the list")

r = call("execute", command="extrude", params={"width": 40, "height": 30, "distance": 10})
check(r.get("ok") and r.get("delta", {}).get("created_bodies") == [0], "execute runs a tool and reports what it created: %s" % r.get("delta"))
r = call("execute", command="extrude", params={"width": 40, "height": 30, "distance": 10}, verbose=False)
check(set(r) == {"ok", "error"}, "verbose:false keeps ok and error only")
try:
    call("execute", command="no_such_thing")
    check(False, "an unknown command is refused")
except RuntimeError:
    check(True, "an unknown command is refused")
try:
    call("execute", command="execute")
    check(False, "execute cannot run itself")
except RuntimeError:
    check(True, "execute cannot run itself")

call("new_document")
call("set_variable", name="w", expr="40")
call("extrude", width=40, height=30, distance=10)
r = call("set_parameter", name="w", expr="50")
check(r.get("ok") and "recomputed" in r and "restored" in r, "set_parameter reports rebuild counts: %s" % {k: r.get(k) for k in ("recomputed", "restored")})

call("new_document")
call("extrude", width=40, height=30, distance=10)
for fmt, magic in (("step", b"ISO-10303"), ("iges", b"S0000001"), ("stl", None), ("obj", b"v ")):
    path = "/tmp/orca-export-check.%s" % fmt
    if os.path.exists(path):
        os.remove(path)
    r = call("export", path=path, format=fmt)
    ok = r.get("ok") and os.path.exists(path) and os.path.getsize(path) > 200
    if ok and magic:
        ok = magic in open(path, "rb").read(400)
    check(ok, "export %s writes a file (%s bytes)" % (fmt, os.path.getsize(path) if os.path.exists(path) else 0))
try:
    call("export", path="/etc/orca-export.stl", format="stl")
    check(False, "export outside the allowed roots is refused")
except RuntimeError:
    check(True, "export outside the allowed roots is refused")

# a feature added while the history is rolled back goes in at the bar, and the bar moves onto it
call("new_document")
call("extrude", width=20, height=20, distance=10)
call("extrude", width=10, height=10, distance=5, boolean="union")
n0 = len(call("describe_scene")["features"])
call("rollback", index=1)
call("fillet", edge=[0], radius=1) if False else call("extrude", width=6, height=6, distance=3, boolean="union")
feats = call("describe_scene")["features"]
check(len(feats) == n0 + 2, "the rolled-back add made %d new features" % (len(feats) - n0))
check([f["type"] for f in feats][:2] == ["Sketch", "Extrude"] and feats[2]["type"] == "Sketch" and feats[3]["type"] == "Extrude",
      "the new sketch + extrude sit right under the bar: %s" % [f["type"] for f in feats])
call("rollback", index=-1)
check(len(call("describe_scene")["features"]) == n0 + 2, "restoring the full history keeps every feature")

print("mcp commands: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
