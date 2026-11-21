#!/usr/bin/env python3
"""A sketch started in the GUI on a clicked face records that face, and follows it when the base changes.
Drives the real event path (ui.click / run_verb / ui.key) against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-sketch-on-face.py [socket]      # default /tmp/orca-cad-mcp.sock
Exit 0 = every assertion held.
"""
import json, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_n = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(60)
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


try:
    call("sketch_cancel")   # leave nothing open from an earlier run
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-face-0.png", view="iso", fit=True)
time.sleep(0.5)
org = call("describe_scene")["modeling_origin"]
top = call("ui.worldToScreen", point=[org[0], org[1], 10.0])
call("ui.click", at=[top["x"], top["y"]])                       # select the top face
call("run_verb", verb="sketch")                                  # S, S: sketch on the picked face
time.sleep(0.5)
st = call("ui.inspect")
check(st["sketch_open"], "a sketch session opened on the face")
call("ui.key", key="C")                                      # the Circle tool, by its shortcut
time.sleep(0.3)
# the sketch view squares to the face: aim at the face centre and a point 60 px away
c = call("ui.inspect")["canvas"]
cx, cy = c["width"] / 2, c["height"] / 2
call("ui.click", at=[cx, cy])
call("ui.click", at=[cx + 60, cy])
time.sleep(0.3)
call("ui.key", key="Escape")      # end the circle tool
call("sketch_commit")             # the toolbar's Confirm: the same commit path the GUI takes
time.sleep(0.8)
feats = call("describe_scene")["features"]
sk = [f for f in feats if f["type"] == "Sketch" or f["type"].lower() == "sketch"]
check(len(sk) >= 1 and "attached_to" in sk[-1], "the committed sketch records its face: %s" % (sk[-1] if sk else feats))
call("screenshot", path="/tmp/ui-face-1.png")

if sk and "attached_to" in sk[-1]:
    idx = sk[-1]["index"]
    call("extrude", sketch=idx, distance=5, boolean="union")
    call("set_variable", name="h", expr="15")
    call("set_feature_expr", feature=1, field="distance", expr="h")
    bb = call("describe_scene")["bodies"][0]["bbox"]
    check(abs(bb["max"][2] - 20.0) < 0.01, "the boss moved up with the face: top at z=%.2f (15 + 5)" % bb["max"][2])
print("ui sketch-on-face:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
