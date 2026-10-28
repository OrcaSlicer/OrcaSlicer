#!/usr/bin/env python3
"""Sketch undo / redo is one step per COMMAND (a shape, a dimension, a constraint, a trim), Trim cuts only the clicked piece and keeps
the rest of the sketch and its constraints, and a dimension the sketch already fixes becomes a reference quote. Real mouse and key
events on the open sketch. Runs against a RUNNING app.

    python3 scripts/CAD/check-ui-sketch-undo.py [socket]
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


def ncons():
    return call("sketch_describe")["constraints"]


try:
    call("sketch_cancel")
except RuntimeError:
    pass
call("new_document")
call("extrude", width=40, height=40, distance=10)
call("screenshot", path="/tmp/ui-snap-0.png", view="iso", fit=True)
time.sleep(0.5)
org = call("describe_scene")["modeling_origin"]
top = call("ui.worldToScreen", point=[org[0], org[1], 10.0])
call("ui.click", at=[top["x"], top["y"]])
call("run_verb", verb="sketch")
time.sleep(0.5)
c = call("ui.inspect")["canvas"]
cx, cy = c["width"] / 2, c["height"] / 2


def draw_line(a, b):
    call("ui.key", key="L")
    call("ui.move", at=a); call("ui.click", at=a); time.sleep(0.5)
    call("ui.move", at=b); call("ui.click", at=b); time.sleep(0.5)
    call("ui.key", key="Escape")


H = []


def solve(M, v):
    n = len(v)
    A = [row[:] + [v[i]] for i, row in enumerate(M)]
    for c in range(n):
        piv = max(range(c, n), key=lambda r: abs(A[r][c]))
        A[c], A[piv] = A[piv], A[c]
        for r in range(n):
            if r != c:
                f = A[r][c] / A[c][c]
                A[r] = [x - f * y for x, y in zip(A[r], A[c])]
    return [A[i][n] / A[i][i] for i in range(n)]


def calibrate(pairs):
    """Sketch point -> pixel, as a homography fitted on four points placed at known pixels (the view is a perspective one, so the scale
    differs from one part of the screen to another and a plain scale + offset is off by many pixels away from the first point)."""
    M, v = [], []
    for (x, y), (u, w) in pairs:
        M.append([x, y, 1, 0, 0, 0, -u * x, -u * y]); v.append(u)
        M.append([0, 0, 0, x, y, 1, -w * x, -w * y]); v.append(w)
    H[:] = solve(M, v) + [1.0]


def scr(p):
    h = H
    d = h[6] * p[0] + h[7] * p[1] + h[8]
    return [(h[0] * p[0] + h[1] * p[1] + h[2]) / d, (h[3] * p[0] + h[4] * p[1] + h[5]) / d]


def points():
    return [e for e in ents() if e["type"] == "point"]


def line(i):
    return [e for e in ents() if e["type"] == "line"][i]


call("ui.key", key="P")                       # four points at known pixels: they calibrate the screen <-> sketch map
corners = [[cx - 300, cy - 180], [cx + 300, cy - 180], [cx + 300, cy + 240], [cx - 300, cy + 240]]
for q in corners:
    call("ui.move", at=q, mods=["shift"]); call("ui.click", at=q, mods=["shift"]); time.sleep(0.4)   # Shift: off the grid, so the point is where it was clicked
call("ui.key", key="Escape")
pts = [q for q in points() if abs(q["p"][0]) + abs(q["p"][1]) > 1e-9][-4:]   # (not the origin point a new sketch starts with)
calibrate([((p["p"][0], p["p"][1]), q) for p, q in zip(pts, corners)])
call("ui.key", key="Escape")


def lines():
    return [e for e in ents() if e["type"] == "line" and abs(e["p0"][0] - e["p1"][0]) + abs(e["p0"][1] - e["p1"][1]) > 1e-6]


def tick():
    """An idle cursor move: the frame between two user actions, so each one is its own undo step."""
    call("ui.move", at=[cx + 3, cy + 300]); time.sleep(0.35)


def undo():
    call("ui.key", key="Z", mods=["ctrl"]); time.sleep(0.45)


def redo():
    call("ui.key", key="Z", mods=["ctrl", "shift"]); time.sleep(0.45)


d0 = call("sketch_describe")
print("describe keys:", sorted(d0.keys()))

# --- 1. undo is one step per command -------------------------------------------------------------------------------------------
call("ui.key", key="Escape")
n0, c0 = len(ents()), ncons()
draw_line([cx - 200, cy - 20], [cx + 200, cy - 20]); tick()
n1 = len(ents())
draw_line([cx, cy - 120], [cx, cy + 120]); tick()
n2 = len(ents())
check(n1 == n0 + 1 and n2 == n0 + 2, "two lines drawn: %d -> %d -> %d entities" % (n0, n1, n2))
c2 = ncons()
call("sketch_constrain", constraints=[{"type": "distance", "a": "%d.start" % (n0 + 1), "b": "%d.end" % (n0 + 1), "value": 77.0}])
tick()
c3 = ncons()
check(c3 == c2 + 1, "a distance constraint was added: %d -> %d" % (c2, c3))
undo()
check(ncons() == c2 and len(ents()) == n2, "Ctrl+Z undoes the constraint alone (entities %d, constraints %d)" % (len(ents()), ncons()))
undo()
check(len(ents()) == n1, "Ctrl+Z again removes the second line: %d entities" % len(ents()))
redo()
check(len(ents()) == n2, "Ctrl+Shift+Z brings the line back: %d entities" % len(ents()))
redo()
check(ncons() == c3, "Ctrl+Shift+Z brings the constraint back: %d constraints" % ncons())
undo()                                           # leave the sketch as "two crossing lines, no length"
check(ncons() == c2, "back at two lines")

# --- 2. trim removes only the clicked piece --------------------------------------------------------------------------------------
draw_line([cx + 130, cy - 120], [cx + 130, cy + 120]); tick()         # a second vertical, right of the first
nl = len(lines())
cons_before = ncons()
hz = [l for l in lines() if abs(l["p0"][1] - l["p1"][1]) < 1e-6 and abs(l["p0"][0] - l["p1"][0]) > 1.0]
check(len(hz) == 1, "one horizontal line to trim: %d" % len(hz))
x_left = min(hz[0]["p0"][0], hz[0]["p1"][0]); x_right = max(hz[0]["p0"][0], hz[0]["p1"][0])
call("ui.key", key="T")
mid = [cx + 60, cy - 20]                                              # between the two verticals
call("ui.move", at=mid); call("ui.click", at=mid); time.sleep(0.6)
hz2 = sorted([l for l in lines() if abs(l["p0"][1] - l["p1"][1]) < 1e-6 and abs(l["p0"][0] - l["p1"][0]) > 1.0], key=lambda l: min(l["p0"][0], l["p1"][0]))
check(len(hz2) == 2, "the trim split the line in two pieces: %d horizontal lines" % len(hz2))
if len(hz2) == 2:
    a0, a1 = sorted([hz2[0]["p0"][0], hz2[0]["p1"][0]]); b0, b1 = sorted([hz2[1]["p0"][0], hz2[1]["p1"][0]])
    check(abs(a0 - x_left) < 1e-6 and abs(b1 - x_right) < 1e-6, "outer ends stay where they were: %.3f..%.3f and %.3f..%.3f" % (a0, a1, b0, b1))
    check(a1 < b0 - 1.0, "the middle piece is gone: %.3f < %.3f" % (a1, b0))
verts = [l for l in lines() if abs(l["p0"][0] - l["p1"][0]) < 1e-6]
check(len(verts) == 2, "both verticals survive: %d" % len(verts))
check(ncons() >= cons_before, "constraints are kept (and the cut points added): %d -> %d" % (cons_before, ncons()))
call("ui.key", key="Escape")
undo()
hz3 = [l for l in lines() if abs(l["p0"][1] - l["p1"][1]) < 1e-6 and abs(l["p0"][0] - l["p1"][0]) > 1.0]
check(len(hz3) == 1 and len(lines()) == nl, "Ctrl+Z undoes the whole trim: %d lines" % len(lines()))

# --- 3. trimming the end beyond the last crossing shortens the line and keeps the rest -----------------------------------
call("ui.key", key="T")
endp = [cx + 180, cy - 20]
call("ui.move", at=endp); call("ui.click", at=endp); time.sleep(0.6)
call("ui.key", key="Escape")
hz4 = [l for l in lines() if abs(l["p0"][1] - l["p1"][1]) < 1e-6 and abs(l["p0"][0] - l["p1"][0]) > 1.0]
check(len(hz4) == 1 and max(hz4[0]["p0"][0], hz4[0]["p1"][0]) < x_right - 1.0 and min(hz4[0]["p0"][0], hz4[0]["p1"][0]) < x_left + 1e-6,
      "the right stub is cut off at the second vertical, the rest stays: %s" % [(round(l["p0"][0], 2), round(l["p1"][0], 2)) for l in hz4])
undo()

print("\n%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
