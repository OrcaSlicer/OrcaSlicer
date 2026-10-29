#!/usr/bin/env python3
"""Split cuts a line in two and keeps them joined, Extend keeps what still holds on the longer line, a bare axis lights up under the
cursor, and a formula or a name bound to a dimension follows its constraint through an edit that erases earlier constraints. Real
mouse events on the open sketch, MCP for the rest. Runs against a RUNNING app.

    python3 scripts/CAD/check-ui-sketch-split.py [socket]
"""
import json, socket, sys, time

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/orca-cad-mcp.sock"
_n = 0

# SketchConstraintType, by position (append-only enum)
COINCIDENT, HORIZONTAL, VERTICAL, DISTANCE, POINT_ON_OBJECT, COLLINEAR = 1, 2, 3, 4, 18, 20


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


def desc():
    time.sleep(0.3)
    return call("sketch_describe")


def lines():
    return [e for e in desc()["entities"] if e["type"] == "line" and abs(e["p0"][0] - e["p1"][0]) + abs(e["p0"][1] - e["p1"][1]) > 1e-6]


def types():
    return [c["type"] for c in desc()["constraint_list"]]


try:
    call("sketch_cancel")
except RuntimeError:
    pass

# --- a formula and a name follow their constraint through an edit --------------------------------------------------------------------
call("new_document")
call("sketch_begin", plane="XY")
call("sketch_add", rect=[0, 0, 40, 30])
call("sketch_select", entities=[0]); call("sketch_set_value", value=40)       # bottom edge: 40
call("sketch_select", entities=[1]); call("sketch_set_value", value=30)       # right edge: 30
sk = call("sketch_commit")["sketch_index"]
dims = call("sketch_dimensions", sketch=sk)["dimensions"]
d30 = [d for d in dims if abs(d["value"] - 30.0) < 1e-6]
check(len(dims) == 2 and len(d30) == 1, "two dimensions, 40 and 30: %s" % [(d["index"], d["value"]) for d in dims])
i30 = d30[0]["index"]
call("set_variable", name="H", expr="30")
call("set_feature_expr", feature=sk, field="dim:%d" % i30, expr="H")
call("name_dimension", sketch=sk, index=i30, name="height")
call("sketch_edit", sketch=sk)
time.sleep(0.5)
call("sketch_delete", entities=[3])                                          # the left edge: its coincidents and its vertical go
call("sketch_commit")
dims = call("sketch_dimensions", sketch=sk)["dimensions"]
bound = [d for d in dims if d["expr"] == "H"]
check(len(bound) == 1 and abs(bound[0]["value"] - 30.0) < 1e-6, "the formula H is still on the 30 dimension: %s" % [(d["index"], d["value"], d["expr"], d["name"]) for d in dims])
check(bound and bound[0]["index"] < i30, "its constraint moved (%d -> %d): the formula went with it" % (i30, bound[0]["index"] if bound else -1))
check(bound and bound[0]["name"] == "height", "and so did its name")
call("set_variable", name="H", expr="25")
dims = call("sketch_dimensions", sketch=sk)["dimensions"]
by = {d["index"]: d for d in dims}
check(bound and abs(by[bound[0]["index"]]["value"] - 25.0) < 1e-6 and sorted(round(d["value"]) for d in dims) == [25, 40],
      "H = 25 drives that dimension and leaves the 40 alone: %s" % [(d["index"], round(d["value"], 3)) for d in dims])

# --- a bare axis lights up under the cursor ------------------------------------------------------------------------------------------------
call("new_document")
org = call("describe_scene")["modeling_origin"]
call("sketch_begin", plane="XY")                 # looking straight down it: the sketch x, y are the world x, y from the modeling origin
time.sleep(0.8)


def scr(x, y):
    p = call("ui.worldToScreen", point=[org[0] + x, org[1] + y, org[2] if len(org) > 2 else 0.0])
    return [p["x"], p["y"]]


call("ui.move", at=scr(12.0, 0.0)); time.sleep(0.4)
check(desc()["hover_axis"] == 1, "the cursor on the X axis lights it: hover_axis=%s" % desc()["hover_axis"])
call("ui.move", at=scr(0.0, 12.0)); time.sleep(0.4)
check(desc()["hover_axis"] == 2, "on the Y axis: hover_axis=%s" % desc()["hover_axis"])
call("ui.move", at=scr(9.0, 9.0)); time.sleep(0.4)
check(desc()["hover_axis"] == 0, "away from both: hover_axis=%s" % desc()["hover_axis"])

# --- Split ------------------------------------------------------------------------------------------------------------------------------------
call("sketch_add", entities=[{"type": "line", "p0": [-16, 8], "p1": [16, 8]}])
n0 = len(lines())
c0 = desc()["constraints"]
call("run_verb", verb="sk_split"); time.sleep(0.4)
check(desc()["tool"] == "split", "the Split row arms the tool: %s" % desc()["tool"])
p = scr(5.0, 8.0)
call("ui.move", at=p); time.sleep(0.3); call("ui.click", at=p); time.sleep(0.6)
ls = lines()
check(len(ls) == n0 + 1, "a click on the line cuts it in two (%d -> %d lines)" % (n0, len(ls)))
cut = [e for e in ls if abs(e["p0"][1] - 8.0) < 1e-6 and abs(e["p1"][1] - 8.0) < 1e-6]
xs = sorted({round(e["p0"][0], 3) for e in cut} | {round(e["p1"][0], 3) for e in cut})
check(len(cut) == 2 and len(xs) == 3 and abs(xs[0] + 16) < 1e-3 and abs(xs[2] - 16) < 1e-3 and abs(xs[1]) < 17,
      "the halves share the cut and keep the ends: x = %s" % xs)
t = types()
check(COINCIDENT in t[c0:], "they stay joined (a Coincident at the cut; a direction relation only if it is not already implied): %s" % t[c0:])
check(desc()["solve_ok"], "the sketch still solves")
call("ui.key", key="Escape")

# --- Extend keeps what still holds ------------------------------------------------------------------------------------------------------------
call("sketch_add", entities=[{"type": "line", "p0": [-16, -8], "p1": [-6, -8]}, {"type": "line", "p0": [4, -14], "p1": [4, -2]}])
ls = lines()
a = [i for i, e in enumerate(desc()["entities"]) if e["type"] == "line" and abs(e["p0"][1] + 8) < 1e-6 and abs(e["p1"][0] + 6) < 1e-6][0]
call("sketch_select", entities=[a]); call("sketch_set_value", value=10.0)       # a length on it: that one does not survive an extension
before = types()
hz_before = before.count(HORIZONTAL)
call("ui.key", key="X"); time.sleep(0.4)
p = scr(-7.0, -8.0)
call("ui.move", at=p); time.sleep(0.3); call("ui.click", at=p); time.sleep(0.6)
e = desc()["entities"][a]
check(abs(e["p1"][0] - 4.0) < 1e-3 and abs(e["p1"][1] + 8.0) < 1e-3, "Extend runs the line out to the other one: end %s" % e["p1"])
after = types()
check(after.count(HORIZONTAL) == hz_before, "its Horizontal is kept (%d -> %d)" % (hz_before, after.count(HORIZONTAL)))
check(after.count(DISTANCE) == before.count(DISTANCE) - 1, "the length it had no longer holds and goes (%d -> %d)" % (before.count(DISTANCE), after.count(DISTANCE)))
check(POINT_ON_OBJECT in after[len(after) - 2:] and after.count(POINT_ON_OBJECT) == before.count(POINT_ON_OBJECT) + 1, "and the new end is held on the line that stopped it")
check(desc()["solve_ok"], "the sketch still solves")
call("ui.key", key="Escape"); call("ui.key", key="Escape")
call("sketch_cancel")

print("ui sketch split:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
