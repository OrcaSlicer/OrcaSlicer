#!/usr/bin/env python3
"""Sketch drawing gestures through the real event path: a two-point shape is drawn click-click OR by one
press-drag-release. Needs a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-sketch-draw.py [socket]      # default /tmp/orca-cad-mcp.sock
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


def ents():
    time.sleep(0.3)
    return call("sketch_describe")["entities"]


try:
    call("sketch_cancel")
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-draw-0.png", view="iso", fit=True)
time.sleep(0.5)
org = call("describe_scene")["modeling_origin"]
top = call("ui.worldToScreen", point=[org[0], org[1], 10.0])
call("ui.click", at=[top["x"], top["y"]])
call("run_verb", verb="sketch")
time.sleep(0.5)
c = call("ui.inspect")["canvas"]
cx, cy = c["width"] / 2, c["height"] / 2
base = len(ents())                                               # a new sketch starts with its origin point

call("ui.key", key="C")                                          # Circle (centre, radius)
call("ui.drag", **{"from": [cx, cy], "to": [cx + 90, cy], "steps": 6})   # press, drag, release = two points
e = ents()
check(len(e) == base + 1, "one press-drag-release draws a whole circle: %d entit(ies)" % len(e))
call("ui.key", key="Escape")
call("ui.key", key="C")
call("ui.click", at=[cx - 150, cy - 100])                         # click, click still works
time.sleep(0.6)
call("ui.click", at=[cx - 150, cy - 40])
e = ents()
check(len(e) == base + 2, "click-click still draws a circle: %d entities" % len(e))
call("ui.key", key="Escape")

# Inference: a line drawn 3 degrees off parallel to an existing one snaps exactly parallel; Ctrl bypasses it.
def draw_line(a, b, mods=None):
    call("ui.key", key="L")
    kw = {"mods": mods} if mods else {}
    call("ui.move", at=a, **kw)
    call("ui.click", at=a, **kw)
    time.sleep(0.6)
    call("ui.move", at=b, **kw)
    call("ui.click", at=b, **kw)
    time.sleep(0.6)
    call("ui.key", key="Escape")

def lines():
    return [x for x in ents() if x.get("type") == "line"]

n0 = len(lines())
draw_line([cx - 200, cy + 200], [cx + 0, cy + 127])               # about 20 degrees (not in the fixed-angle table)
draw_line([cx - 200, cy + 260], [cx + 0, cy + 175])               # 3 degrees off parallel
L = lines()[n0:]
check(len(L) == 2, "two lines drawn: %d" % len(L))
def dirv(e):
    p0, p1 = e["p0"], e["p1"]
    d = [p1[0] - p0[0], p1[1] - p0[1]]
    n = (d[0] ** 2 + d[1] ** 2) ** 0.5
    return [d[0] / n, d[1] / n]
if len(L) == 2:
    a, b = dirv(L[0]), dirv(L[1])
    check(abs(a[0] * b[1] - a[1] * b[0]) < 1e-6, "the second line snapped exactly parallel (cross %.2e)" % (a[0] * b[1] - a[1] * b[0]))
n1 = len(lines())
draw_line([cx - 200, cy + 320], [cx + 0, cy + 235], mods=["ctrl"])
L2 = lines()[n1:]
on_grid = lambda e: all(abs(v - round(v)) < 1e-9 for v in e["p1"])
if len(L) == 2 and len(L2) == 1:
    check(not on_grid(L[1]), "the guided end is placed off the grid, on the guide direction: %s" % L[1]["p1"])
    check(on_grid(L2[0]), "with Ctrl held the guide is off and the grid decides: %s" % L2[0]["p1"])
# Type-first length: a digit typed with the first point down completes the segment and fills its Length field.
n2 = len(lines())
call("ui.key", key="L")
call("ui.move", at=[cx + 220, cy + 300])
call("ui.click", at=[cx + 220, cy + 300])
time.sleep(0.6)
call("ui.move", at=[cx + 400, cy + 300])
time.sleep(0.4)
call("ui.key", key="5")
time.sleep(0.8)
call("ui.text", text="0")
time.sleep(0.4)
call("ui.key", key="Return")
time.sleep(0.6)
call("ui.key", key="Escape")
L3 = lines()[n2:]
check(len(L3) == 1 and abs(L3[0]["length"] - 50.0) < 1e-6, "typing 5,0,Enter after the first point makes a 50 mm line: %s" % [round(x["length"], 3) for x in L3])
# A drag of a point is ONE undo step: Ctrl+Z puts it back, and does not delete anything.
n3 = len(lines())
a, b = [cx + 250, cy - 250], [cx + 400, cy - 250]
draw_line(a, b)
call("ui.key", key="Escape")                               # leave the Line tool: back to Select
mine = lines()[n3:]
check(len(mine) == 1, "a line to drag was drawn")
if len(mine) == 1:
    before = mine[0]["p1"]
    call("ui.drag", **{"from": b, "to": [b[0] + 30, b[1] + 60], "steps": 6})
    after = [e for e in ents() if e["index"] == mine[0]["index"]][0]["p1"]
    check(abs(after[0] - before[0]) + abs(after[1] - before[1]) > 0.5, "dragging the end moved it: %s -> %s" % (before, after))
    cnt = len(ents())
    call("ui.key", key="Z", mods=["ctrl"])
    time.sleep(0.4)
    back = [e for e in ents() if e["index"] == mine[0]["index"]]
    check(len(ents()) == cnt and back and abs(back[0]["p1"][0] - before[0]) + abs(back[0]["p1"][1] - before[1]) < 1e-6,
          "Ctrl+Z undoes the drag and removes nothing: %s" % (back[0]["p1"] if back else None))
    call("ui.key", key="Z", mods=["ctrl", "shift"])
    time.sleep(0.4)
    redo = [e for e in ents() if e["index"] == mine[0]["index"]]
    check(redo and abs(redo[0]["p1"][0] - after[0]) + abs(redo[0]["p1"][1] - after[1]) < 1e-6, "Ctrl+Shift+Z puts the drag back: %s" % (redo[0]["p1"] if redo else None))

# Press-drag-release gives the first two points of every shape that has them; a longer shape is finished with clicks as usual.
def drag_draw(key, extra=(), name=""):
    n0 = len(ents())
    call("ui.key", key=key)
    call("ui.drag", **{"from": [cx - 300, cy - 200], "to": [cx - 160, cy - 160], "steps": 6})
    time.sleep(0.5)
    for q in extra:
        call("ui.move", at=q); call("ui.click", at=q); time.sleep(0.5)
    call("ui.key", key="Escape")
    return len(ents()) - n0

check(drag_draw("L") == 1, "Line: one press-drag-release makes one segment")
check(drag_draw("R") == 4, "Rectangle: one press-drag-release makes the four sides")
check(drag_draw("S", extra=[[cx - 230, cy - 120]]) > 0, "Slot: drag the centre line, click the width")
check(drag_draw("E", extra=[[cx - 230, cy - 120]]) > 0, "Ellipse: drag the major axis, click the minor point")
check(drag_draw("A", extra=[[cx - 230, cy - 120]]) > 0, "Arc: drag start-end, click the third point")

# Type-first for the shapes with a first field: a digit with the first point down completes the shape and types into that field
# (rectangle: width, Tab, height; circle: radius).
def typed_first(key):
    n0 = len(ents())
    call("ui.key", key=key)
    call("ui.move", at=[cx - 200, cy - 100]); call("ui.click", at=[cx - 200, cy - 100]); time.sleep(0.5)
    call("ui.move", at=[cx - 100, cy - 60]); time.sleep(0.4)
    call("ui.key", key="5"); time.sleep(0.8)
    call("ui.text", text="0"); time.sleep(0.4)
    if key == "R":
        call("ui.key", key="Tab"); time.sleep(0.4)
        call("ui.text", text="30"); time.sleep(0.4)
    call("ui.key", key="Return"); time.sleep(0.8)
    es = ents()[n0:]
    call("ui.key", key="Escape")
    return es

es = typed_first("R")
if len(es) == 4:
    xs = [v for e in es for v in (e["p0"][0], e["p1"][0])]; ys = [v for e in es for v in (e["p0"][1], e["p1"][1])]
    check(abs((max(xs) - min(xs)) - 50) < 1e-6 and abs((max(ys) - min(ys)) - 30) < 1e-6, "typing 5, 0, Tab, 3, 0, Enter after the first corner makes a 50 x 30 rectangle: %.3f x %.3f" % (max(xs) - min(xs), max(ys) - min(ys)))
else:
    check(False, "type-first rectangle made %d entities" % len(es))
es = typed_first("C")
check(len(es) == 1 and abs(es[0]["radius"] - 50) < 1e-6, "typing 5, 0, Enter after the centre makes a circle of radius 50: %s" % [e.get("radius") for e in es])

# Tab goes round the fields: after the height, Tab is back at the width (Enter would end the chain), and Shift+Tab goes the other way.
n0 = len(ents())
call("ui.key", key="R")
call("ui.move", at=[cx - 200, cy - 100]); call("ui.click", at=[cx - 200, cy - 100]); time.sleep(0.5)
call("ui.move", at=[cx - 100, cy - 60]); time.sleep(0.4)
call("ui.key", key="5"); time.sleep(0.8)
call("ui.text", text="0"); time.sleep(0.4)
call("ui.key", key="Tab"); time.sleep(0.5)          # width 50 accepted, now the height
call("ui.text", text="30"); time.sleep(0.4)
call("ui.key", key="Tab"); time.sleep(0.6)          # height 30 accepted; the chain wraps back to the width
call("ui.text", text="60"); time.sleep(0.4)
call("ui.key", key="Return"); time.sleep(0.6)       # width 60 accepted; Enter moves to the height ...
call("ui.key", key="Return"); time.sleep(0.8)       # ... and ends the chain on it
es = ents()[n0:]
xs = [v for e in es for v in (e["p0"][0], e["p1"][0])]; ys = [v for e in es for v in (e["p0"][1], e["p1"][1])]
check(len(es) == 4 and abs((max(xs) - min(xs)) - 60) < 1e-6 and abs((max(ys) - min(ys)) - 30) < 1e-6,
      "Tab after the last field wraps to the first: a 60 x 30 rectangle (%.3f x %.3f)" % (max(xs) - min(xs), max(ys) - min(ys)))
call("ui.key", key="Escape")
call("sketch_cancel")
print("ui sketch draw:", "PASS" if not fails else "%d FAIL" % len(fails))
sys.exit(1 if fails else 0)
