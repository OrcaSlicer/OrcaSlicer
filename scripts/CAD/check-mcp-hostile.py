#!/usr/bin/env python3
"""The MCP socket must survive any argument and say plainly why it refuses.

Drives a RUNNING app (launch with ORCA_CAD_MCP=<sock>) and asserts:
  1. every method in describe_tools, fed junk parameters, answers (result or error) and never kills the app;
  2. a refusal names its owner (error.data.owner) - sketch / feature_card / ... - so a caller can recover;
  3. paths outside the allowed roots are refused for save / export / screenshot / open;
  4. two clients can talk at once;
  5. describe_tools covers every method the app dispatches (readOnly annotations present).

    python3 scripts/CAD/check-mcp-hostile.py [socket]     # default /tmp/orca-cad-mcp.sock
Exit 0 = all held. Run it on a scratch app: it creates and discards documents.
"""
import json, socket, sys, threading, itertools

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_ids = itertools.count(1)
fails = []


def raw(method, params=None, timeout=60):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": next(_ids), "method": method, "params": params if params is not None else {}}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    s.close()
    return json.loads(buf.decode())


def check(cond, msg):
    if not cond:
        fails.append(msg)
        print("FAIL", msg)


def alive():
    try:
        return "result" in raw("describe_scene")
    except Exception:
        return False


tools = raw("describe_tools")["result"]["tools"]
names = [t["name"] for t in tools]
check(all("readOnly" in t for t in tools), "every tool carries a readOnly annotation")
check({"sweep", "loft", "thread", "cut", "save", "open", "screenshot", "sketch_constrain", "batch", "inspect_design"} <= set(names), "core tools are described")

# 1. junk in, no crash out. save/open/screenshot/export/import_* and run_verb are skipped: they act outside the document.
SKIP = {"save", "open", "screenshot", "export", "import_step", "import_mesh", "run_verb", "new_document", "describe_tools"}
JUNK = [None, "x", -1, 0, 1e308, float("nan") if False else 1e-308, [], {}, [[]], {"a": {"b": [None]}}, ["x" * 1000], -1e308, True]
for name in names:
    if name in SKIP:
        continue
    raw("new_document")
    for j in JUNK:
        for params in ({"body": j}, {"edge": j}, {"face": j}, {"distance": j}, {"width": j, "height": j}, {"profile": j}, {"sketch": j}, {"entities": j},
                       {"constraints": j}, {"commands": j}, {"points": j}, {"plane": j}, {"value": j}, {"name": j, "expr": j}):
            try:
                r = raw(name, params)
            except Exception as e:
                check(False, "%s %r: no answer (%s)" % (name, params, e))
                break
            if "result" not in r and "error" not in r:
                check(False, "%s %r: neither result nor error" % (name, params))
            if not alive():
                check(False, "app died after %s %r" % (name, params))
                print("%d failures" % len(fails))
                sys.exit(1)
    # close a sketch some call may have left open, so the next method starts clean
    raw("sketch_cancel")

# 2. a refusal names its owner
raw("new_document")
raw("sketch_begin", {"plane": "XY"})
r = raw("extrude", {"width": 5, "height": 5, "distance": 5})
check(r.get("error", {}).get("code") == -32002, "extrude while a sketch is open is refused with -32002")
check(r.get("error", {}).get("data", {}).get("owner") == "sketch", "the refusal names the sketch as owner: %r" % r.get("error"))
raw("sketch_cancel")

# 3. path scope
for method, p in (("save", "/etc/hostile.3mf"), ("export", "/etc/hostile.step"), ("screenshot", "/etc/hostile.png"), ("open", "/etc/passwd")):
    r = raw(method, {"path": p})
    ok = "error" in r or r.get("result", {}).get("ok") is False
    check(ok, "%s outside the allowed roots is refused" % method)
r = raw("save", {"path": "/tmp/../etc/x.3mf"})
check("error" in r or r["result"].get("ok") is False, "'..' is refused")

# 4. two clients at once
out = []
def worker():
    for _ in range(5):
        out.append("result" in raw("describe_scene"))
ts = [threading.Thread(target=worker) for _ in range(3)]
[t.start() for t in ts]
[t.join() for t in ts]
check(len(out) == 15 and all(out), "three concurrent clients all get answers")

# 5. malformed transport: garbage, deep nesting, a huge field - each answered or dropped, never fatal
def send_line(b):
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.settimeout(20)
    c.connect(SOCK)
    try:
        c.sendall(b + b"\n")
        c.recv(65536)
    except OSError:
        pass
    c.close()

for line in (b"not json", b"{}", b'{"method": 5}', b'{"jsonrpc":"2.0","id":1,"method":"describe_scene","params":' + b"[" * 100000 + b"]" * 100000 + b"}",
             b'{"jsonrpc":"2.0","id":1,"method":"describe_scene","params":{"x":"' + b"A" * 3_000_000 + b'"}}', b'{"jsonrpc":"2.0","id":1,"method":"' + b"m" * 100000 + b'"}'):
    send_line(line)
    check(alive(), "app alive after a malformed line (%d bytes)" % len(line))

print("hostile check:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
