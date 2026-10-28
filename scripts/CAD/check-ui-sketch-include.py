#!/usr/bin/env python3
"""Include (U) brings the model's in-plane edges into the sketch as fixed construction lines; an endpoint drawn on the middle of an
existing line is held there; a dimension the sketch already fixes becomes a reference quote; W / Y / Z start Move / Rotate / Scale.
Real mouse and key events on the open sketch. Runs against a RUNNING app.

    python3 scripts/CAD/check-ui-sketch-include.py [socket]
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




def tick():
    call("ui.move", at=[cx + 3, cy + 300]); time.sleep(0.35)


def lines():
    return [e for e in ents() if e["type"] == "line" and abs(e["p0"][0] - e["p1"][0]) + abs(e["p0"][1] - e["p1"][1]) > 1e-6]


def tool():
    return call("sketch_describe")["tool"]


# --- Include --------------------------------------------------------------------------------------------------------------------
call("ui.key", key="Escape")
n0, c0 = len(ents()), ncons()
call("ui.key", key="U"); time.sleep(0.6)
inc = [e for e in ents()[n0:]]
check(len(inc) == 4 and all(e["type"] == "line" for e in inc), "U includes the four top edges of the box as lines: %s" % [e["type"] for e in inc])
check(all(e.get("construction") for e in inc), "included edges are construction")
check(ncons() == c0 + 8, "each included line is held at both ends: %d -> %d constraints" % (c0, ncons()))
call("ui.key", key="U"); time.sleep(0.4)
check(len(ents()) == n0 + 4, "a second U adds nothing (already included): %d entities" % len(ents()))

# --- an endpoint drawn on the middle of an included edge is held there -----------------------------------------------------------
top = max(inc, key=lambda e: max(e["p0"][1], e["p1"][1]) if abs(e["p0"][1] - e["p1"][1]) < 1e-6 else -1e9)
mx = 0.5 * (top["p0"][0] + top["p1"][0]); my = top["p0"][1]
a, b = scr([mx + 7.0, my - 4.0]), scr([mx + 7.0, my - 12.0])
cA = ncons()
draw_line(a, b); tick()                                    # a free vertical line: only its Vertical is inferred
free_delta = ncons() - cA
cB = ncons()
a, b = scr([mx, my]), scr([mx, my - 10.0])
draw_line(a, b); tick()                                    # the same, starting on the middle of the top edge
mid_delta = ncons() - cB
check(free_delta == 1 and mid_delta == 2, "drawn from the middle of an edge it gains a Midpoint constraint (+%d, a free line +%d)" % (mid_delta, free_delta))

# --- a dimension the sketch already fixes becomes a reference quote -----------------------------------------------------------------
call("run_verb", verb="sk_dimension"); time.sleep(0.4)
cD = ncons()
p = scr([mx - 8.0, my])                                    # on the top (fixed) edge, clear of the line just drawn
call("ui.move", at=p); call("ui.click", at=p); time.sleep(0.4)
q = scr([mx - 8.0, my + 6.0])
call("ui.move", at=q); call("ui.click", at=q); time.sleep(0.6)
d = call("sketch_describe")
check(ncons() == cD and d["solve_ok"], "a length the sketch already fixes adds no constraint and the sketch stays solved (%d -> %d, solve_ok=%s)" % (cD, ncons(), d["solve_ok"]))
call("ui.key", key="Escape"); call("ui.key", key="Escape")

# --- the new keys ------------------------------------------------------------------------------------------------------------------
for key, want in (("W", "Move"), ("Y", "Rotate"), ("Z", "Scale")):
    call("ui.key", key=key); time.sleep(0.3)
    t = tool()
    check(str(t).lower().startswith(want.lower()), "%s starts %s (tool = %s)" % (key, want, t))
    call("ui.key", key="Escape")

print("\n%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
