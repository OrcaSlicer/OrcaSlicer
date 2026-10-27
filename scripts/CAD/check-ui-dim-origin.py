#!/usr/bin/env python3
"""The Dimension tool measures from the bare sketch origin: click the origin, then a circle centre, and the quote lands on a point held
on the origin (made on the spot when the sketch has none). Real mouse events on the open sketch. Runs against a RUNNING app.

    python3 scripts/CAD/check-ui-dim-origin.py [socket]
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
call("sketch_delete", entities=[0])              # a sketch with no origin point: the click on the bare origin has to make one
check(not [p for p in points() if abs(p["p"][0]) < 1e-9 and abs(p["p"][1]) < 1e-9], "no point on the origin to start with")
CC = [cx + 120, cy + 100]
call("ui.key", key="C")
call("ui.move", at=CC); call("ui.click", at=CC); time.sleep(0.5)
call("ui.move", at=[CC[0] + 30, CC[1]]); call("ui.click", at=[CC[0] + 30, CC[1]]); time.sleep(0.5)
call("ui.key", key="Escape")
base = ncons()
circ = [e for e in ents() if e["type"] == "circle"][0]
call("run_verb", verb="sk_dimension")
time.sleep(0.4)
o = scr([0.0, 0.0])
call("ui.move", at=o); call("ui.click", at=o); time.sleep(0.4)
p = scr(circ["center"])
call("ui.move", at=p); call("ui.click", at=p); time.sleep(0.6)
call("ui.key", key="Return")
time.sleep(0.5)
on_origin = [e for e in ents() if e["type"] == "point" and abs(e["p"][0]) < 1e-9 and abs(e["p"][1]) < 1e-9]
check(len(on_origin) == 1, "clicking the bare origin made a point on it (%d)" % len(on_origin))
check(ncons() >= base + 2, "and the quote is a constraint on top of the origin's own (constraints %d -> %d)" % (base, ncons()))
call("sketch_cancel")
print("dim origin: %s" % ("PASS" if not fails else "FAIL (%d)" % len(fails)))
