#!/usr/bin/env python3
"""A dragged sketch point let go on another point, a midpoint or a curve is placed exactly there and keeps the relation (coincident,
midpoint, on object); Ctrl bypasses it. Real mouse events on the open sketch. Runs against a RUNNING app with the Design tab visible.

    python3 scripts/CAD/check-ui-drag-snap.py [socket]
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
pts = points()
calibrate([((p["p"][0], p["p"][1]), q) for p, q in zip(pts, corners)])
A0, A1 = [cx - 250, cy - 150], [cx - 50, cy - 150]
draw_line(A0, A1)
call("ui.key", key="Escape")


def draw_circle(c, r):
    call("ui.key", key="C")
    call("ui.move", at=c); call("ui.click", at=c); time.sleep(0.5)
    call("ui.move", at=[c[0] + r, c[1]]); call("ui.click", at=[c[0] + r, c[1]]); time.sleep(0.5)
    call("ui.key", key="Escape")


def circle(i):
    return [e for e in ents() if e["type"] == "circle"][i]


draw_circle([cx - 100, cy + 120], 40)
draw_circle([cx + 100, cy + 120], 40)
draw_circle([cx + 200, cy + 250], 40)
call("ui.key", key="Escape")                  # leave the Circle tool: back to Select, where a point can be grabbed
a = line(0)
base = ncons()

# 1. a circle's centre dropped near the end of line A: coincident, exactly
k = circle(0)
call("ui.drag", **{"from": scr(k["center"]), "to": [scr(a["p1"])[0] + 3, scr(a["p1"])[1] + 3], "steps": 8})
k = circle(0)
check(abs(k["center"][0] - a["p1"][0]) < 1e-6 and abs(k["center"][1] - a["p1"][1]) < 1e-6, "dropped near an end it lands exactly on it: %s vs %s" % (k["center"], a["p1"]))
check(ncons() == base + 1, "and keeps a relation (constraints %d -> %d)" % (base, ncons()))

# 2. another centre dropped near the middle of A: midpoint
base = ncons()
ma = [(a["p0"][0] + a["p1"][0]) / 2, (a["p0"][1] + a["p1"][1]) / 2]
mid = scr(ma)
k2 = circle(1)
call("ui.drag", **{"from": scr(k2["center"]), "to": [mid[0] + 2, mid[1] + 3], "steps": 8})
k2 = circle(1)
check(abs(k2["center"][0] - ma[0]) < 1e-6 and abs(k2["center"][1] - ma[1]) < 1e-6, "dropped near the middle it lands on the midpoint: %s vs %s" % (k2["center"], ma))
check(ncons() > base, "and keeps a relation (constraints %d -> %d)" % (base, ncons()))

# 3. Ctrl bypasses it: the third centre dropped near A's start does not snap or constrain
base = ncons()
k3 = circle(2)
call("ui.drag", **{"from": scr(k3["center"]), "to": [scr(a["p0"])[0] + 4, scr(a["p0"])[1] + 4], "steps": 8, "mods": ["ctrl"]})
k3 = circle(2)
check(not (abs(k3["center"][0] - a["p0"][0]) < 1e-6 and abs(k3["center"][1] - a["p0"][1]) < 1e-6), "with Ctrl held the centre is not pulled onto the point: %s" % k3["center"])
check(ncons() == base, "and no relation is added (constraints %d)" % ncons())
call("sketch_cancel")
print("drag snap: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
