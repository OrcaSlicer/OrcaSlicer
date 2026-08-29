#!/usr/bin/env python3
"""A ladder of 2D sketches of increasing complexity, judged the way a person judges them.

WHY NOT AREA. Area is derived and no one can confirm it by looking. What a human checks at a
glance, and can be exactly right or exactly wrong about, is:

    VERTEX    is the corner where I said it is
    LENGTH    is the side the length I gave it
    ARC       is the radius the radius I gave it
    TANGENT   does the straight run into the curve smoothly, or is there a kink
    SYMMETRY  is the mirrored half the exact reflection of the half I drew
    CLOSED    is it one closed loop, or does it just look like one

Every rung asserts those. Area appears only as a cross-check, never as the verdict.

Entirely 2D: sketch entities only, no extrude, revolve or any solid feature.

    SNAPORCA_MCP=/tmp/mcp.sock <binary>
    python3 scripts/CAD/check-sketch-engine.py [socket]

Exit 0 = every rung held. Otherwise the first broken property is named and the run stops.
"""
import json, math, socket, sys

SOCK = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mcp.sock"
EPS = 1e-9
_n = 0
_fail = 0


def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": _n, "method": method,
                           "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    r = json.loads(buf.decode().strip())
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']['message']}")
    return r["result"]


def check(kind, cond, what):
    global _fail
    if cond:
        print(f"    {kind:9s} ok    {what}")
    else:
        print(f"    {kind:9s} FAIL  {what}", file=sys.stderr)
        _fail += 1


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def pt_near(p, q, tol=1e-6):
    return math.hypot(p[0] - q[0], p[1] - q[1]) <= tol


def fresh(plane="XY"):
    try:
        call("sketch_cancel")
    except Exception:
        pass
    call("sketch_begin", plane=plane)


def ents():
    return call("sketch_describe")["entities"]


def rep():
    return call("sketch_describe")


def endpoints(e):
    """Both ends of an open curve, as tuples. Closed curves have none."""
    if "p0" not in e or "p1" not in e:
        return ()
    return tuple(e["p0"]), tuple(e["p1"])


def tangent(e, at_end):
    """Unit tangent of entity e at one of its ends, pointing ALONG the curve (p0->p1)."""
    if e["type"] == "line":
        dx = e["p1"][0] - e["p0"][0]
        dy = e["p1"][1] - e["p0"][1]
    else:  # arc
        a = e["start_angle"] if not at_end else e["end_angle"]
        ccw = e["end_angle"] >= e["start_angle"]
        # d/dtheta (cos, sin) = (-sin, cos), reversed when the sweep is clockwise
        dx, dy = -math.sin(a), math.cos(a)
        if not ccw:
            dx, dy = -dx, -dy
    n = math.hypot(dx, dy)
    return (dx / n, dy / n)


def tangent_at_point(e, p):
    """Unit tangent of e at whichever of its ends is p, oriented leaving that point."""
    p0, p1 = endpoints(e)
    if pt_near(p0, p):
        t = tangent(e, False)
        return t
    t = tangent(e, True)
    return (-t[0], -t[1])          # leaving p1 means going back along the curve


def smooth(e1, e2, p):
    """G1 at shared point p: the tangent leaving e1 is opposite the tangent leaving e2."""
    a = tangent_at_point(e1, p)
    b = tangent_at_point(e2, p)
    return abs(a[0] * (-b[0]) - 0) >= 0 and abs(a[0] * b[1] - a[1] * b[0]) <= 1e-6


def closed_one_loop(r, voids=0):
    return (r["buildable"] and r["open_ends"] == []
            and len([l for l in r["closed_loops"] if not any(
                i in h["holes"] for h in r["closed_loops"] for i in [])]) >= 1)


def outer_loop(r):
    """The loop that encloses the others (or the only one)."""
    if not r["closed_loops"]:
        return None
    return max(r["closed_loops"], key=lambda l: abs(l["area"]))


# ─────────────────────────────────────────────────────────────────────────────
print("RUNG 1 — rectangle: four corners, four lengths, four right angles")
fresh()
W, H = 80.0, 50.0
call("sketch_add", rect=[0, 0, W, H])
r = rep()
es = r["entities"]
corners = {(0, 0), (W, 0), (W, H), (0, H)}
got = set()
for e in es:
    got.add(tuple(e["p0"]))
    got.add(tuple(e["p1"]))
check("VERTEX", all(any(pt_near(c, g) for g in got) for c in corners),
      f"all four corners exactly where asked {sorted(corners)}")
lens = sorted(round(e["length"], 9) for e in es)
check("LENGTH", lens == sorted([W, W, H, H]), f"sides are {W}/{H} twice each (got {lens})")
# right angles: consecutive sides meet at 90 degrees
ang_ok = True
for e in es:
    for f in es:
        if e is f:
            continue
        for p in endpoints(e):
            if any(pt_near(p, q) for q in endpoints(f)):
                a, b = tangent_at_point(e, p), tangent_at_point(f, p)
                if abs(a[0] * b[0] + a[1] * b[1]) > 1e-6:
                    ang_ok = False
check("ANGLE", ang_ok, "every corner is exactly 90 degrees")
check("CLOSED", r["buildable"] and r["open_ends"] == [], "one closed loop, no free ends")

print("\nRUNG 2 — a circular void inside it")
call("sketch_add", type="circle", center=[W / 2, H / 2], radius=12)
r = rep()
c = [e for e in r["entities"] if e["type"] == "circle"][0]
check("VERTEX", pt_near(tuple(c["center"]), (W / 2, H / 2)), "void centred exactly where asked")
check("ARC", near(c["radius"], 12), f"void radius exactly 12 (got {c['radius']})")
out = outer_loop(r)
check("CLOSED", len(out["holes"]) == 1, "the rectangle encloses exactly one void")
check("CLOSED", r["buildable"] and r["open_ends"] == [], "still closed with the void present")

print("\nRUNG 3 — stadium: straights running into caps, tangent at every junction")
fresh()
L, R = 50.0, 15.0
call("sketch_add", entities=[
    {"type": "line", "p0": [-L, -R], "p1": [L, -R]},
    {"type": "arc", "center": [L, 0], "radius": R,
     "start_angle": -math.pi / 2, "end_angle": math.pi / 2},
    {"type": "line", "p0": [L, R], "p1": [-L, R]},
    {"type": "arc", "center": [-L, 0], "radius": R,
     "start_angle": math.pi / 2, "end_angle": 3 * math.pi / 2},
])
r = rep()
es = r["entities"]
check("CLOSED", r["buildable"] and r["open_ends"] == [], "one closed loop, no free ends")
arcs = [e for e in es if e["type"] == "arc"]
check("ARC", all(near(a["radius"], R) for a in arcs), f"both caps exactly R={R}")
check("LENGTH", all(near(e["length"], 2 * L) for e in es if e["type"] == "line"),
      f"both straights exactly {2*L}")
# tangency at all four line/arc junctions
tang = True
for a in arcs:
    for p in endpoints(a):
        mates = [e for e in es if e is not a and any(pt_near(p, q) for q in endpoints(e))]
        for m in mates:
            if not smooth(a, m, p):
                tang = False
check("TANGENT", tang, "straight meets cap smoothly at all four junctions (no kink)")

print("\nRUNG 4 — mirror: the reflected half is the exact reflection")
fresh()
half = [
    {"type": "line", "p0": [0, -R], "p1": [L, -R]},
    {"type": "arc", "center": [L, 0], "radius": R,
     "start_angle": -math.pi / 2, "end_angle": math.pi / 2},
    {"type": "line", "p0": [L, R], "p1": [0, R]},
]
call("sketch_add", entities=half)
r = rep()
check("CLOSED", not r["buildable"] and len(r["open_ends"]) == 2,
      f"half profile is correctly OPEN, both ends named {r['open_ends']}")
call("sketch_select", entities=[0, 1, 2])
call("sketch_mirror", axis_a=[0, 0], axis_b=[0, 1])
r = rep()
es = r["entities"]
check("CLOSED", r["buildable"] and r["open_ends"] == [], "mirroring closed the loop")
# every source vertex must have its exact reflection present
src = []
for e in es[:3]:
    src += [tuple(e["p0"]), tuple(e["p1"])]
allv = []
for e in es:
    allv += [tuple(e["p0"]), tuple(e["p1"])]
sym = all(any(pt_near((-x, y), v) for v in allv) for (x, y) in src)
check("SYMMETRY", sym, "every vertex has its exact mirror twin across x=0")
mirrored_arc = [e for e in es[3:] if e["type"] == "arc"]
check("ARC", mirrored_arc and near(mirrored_arc[0]["radius"], R)
      and pt_near(tuple(mirrored_arc[0]["center"]), (-L, 0)),
      f"mirrored cap keeps R={R} and lands at (-{L}, 0)")

print("\nRUNG 5 — offset: every curve moves by exactly d, and it stays closed")
d = 4.0
call("sketch_select", entities=list(range(len(es))))
call("sketch_offset", distance=-d)          # -d = outward for this CCW loop
r = rep()
new = r["entities"][len(es):]
check("CLOSED", r["buildable"] and r["open_ends"] == [], "offset result is closed")
off_arcs = [e for e in new if e["type"] == "arc"]
check("ARC", all(near(a["radius"], R + d) for a in off_arcs),
      f"each cap radius grew by exactly {d} -> {R+d}")
off_lines = [e for e in new if e["type"] == "line"]
check("VERTEX", all(near(abs(e["p0"][1]), R + d) for e in off_lines),
      f"each straight moved out to |y| = {R+d} exactly")

print("\nRUNG 6 — a gap is found by coordinate, then closed by a real constraint")
fresh()
call("sketch_add", entities=[
    {"type": "line", "p0": [0, 0], "p1": [60, 0]},
    {"type": "line", "p0": [60, 0], "p1": [60, 40]},
    {"type": "line", "p0": [60, 40], "p1": [0, 40]},
    {"type": "line", "p0": [0, 40], "p1": [0.35, 0]},      # 0.35 mm short
])
r = call("sketch_validate", tolerance=1.0)
check("CLOSED", not r["buildable"] and len(r["open_ends"]) == 2,
      f"the gap is reported, both free ends named {r['open_ends']}")
dof0 = r["dof"]
r = call("sketch_heal", tolerance=1.0)
check("CLOSED", r["buildable"] and r["open_ends"] == [], "healed into a closed loop")
check("VERTEX", r["welded"] == 1, "exactly one pair of vertices welded")
check("ANGLE", r["dof"] < dof0,
      f"the weld is a real constraint, not a nudge: DoF {dof0} -> {r['dof']}")
es = ents()
check("VERTEX", pt_near(tuple(es[3]["p1"]), tuple(es[0]["p0"])),
      "the two ends are now the same point")

print("\nRUNG 7 — the composite: mirrored, tangent, two voids, all at once")
fresh()
call("sketch_add", entities=half)
call("sketch_select", entities=[0, 1, 2])
call("sketch_mirror", axis_a=[0, 0], axis_b=[0, 1])
call("sketch_add", type="circle", center=[-25, 0], radius=6)
call("sketch_add", type="circle", center=[25, 0], radius=6)
r = rep()
es = r["entities"]
out = outer_loop(r)
check("CLOSED", r["buildable"] and r["open_ends"] == [], "one closed outer loop, no free ends")
check("CLOSED", len(out["holes"]) == 2, "it encloses exactly two voids")
circles = [e for e in es if e["type"] == "circle"]
check("ARC", all(near(c["radius"], 6) for c in circles), "both voids exactly R=6")
check("SYMMETRY", pt_near(tuple(circles[0]["center"]), (-25, 0))
      and pt_near(tuple(circles[1]["center"]), (25, 0)),
      "the voids sit symmetrically at x = -25 and +25")
tang = True
for a in [e for e in es if e["type"] == "arc"]:
    for p in endpoints(a):
        for m in [e for e in es if e is not a and any(pt_near(p, q) for q in endpoints(e))]:
            if not smooth(a, m, p):
                tang = False
check("TANGENT", tang, "every straight-to-cap junction is still smooth")
exact = 2 * L * 2 * R + math.pi * R * R
check("LENGTH", near(out["area"], exact, 1e-6),
      f"cross-check: enclosed area {out['area']:.4f} = 2L*2R + pi*R^2 = {exact:.4f}")

print("\nRUNG 8 — a real drawing: StudyCadCam MPD5, the pin's revolve half-profile")
# Ø27 x 95 pin: C1 chamfer on the left end, cylinder to a corner at x=85, an R5 fillet into a
# cone at 23 degrees to the axis, right face at x=95. Interpretation stated so the rung is
# reproducible: 85 is to the CORNER, 23 deg is to the AXIS, C1 is 1 x 45.
fresh()
RAD, LEN, TX, ANG, RF, CH = 13.5, 95.0, 85.0, math.radians(23), 5.0, 1.0
t  = RF * math.tan(ANG / 2)
ax, ay = TX - t, RAD                      # fillet tangent point on the cylinder
cx, cy = ax, RAD - RF                     # fillet centre
bx, by = TX + t * math.cos(-ANG), RAD + t * math.sin(-ANG)   # tangent point on the cone
ey = by - (LEN - bx) * math.tan(ANG)      # where the cone meets the right face
call("sketch_add", entities=[
    {"type": "line", "p0": [0, 0],            "p1": [0, RAD - CH]},        # left face
    {"type": "line", "p0": [0, RAD - CH],     "p1": [CH, RAD]},            # C1 chamfer
    {"type": "line", "p0": [CH, RAD],         "p1": [ax, ay]},             # cylinder top
    {"type": "arc",  "center": [cx, cy], "radius": RF,
     "start_angle": math.pi / 2, "end_angle": math.pi / 2 - ANG},          # R5 fillet
    {"type": "line", "p0": [bx, by],          "p1": [LEN, ey]},            # 23 deg cone
    {"type": "line", "p0": [LEN, ey],         "p1": [LEN, 0]},             # right face
    {"type": "line", "p0": [LEN, 0],          "p1": [0, 0]},               # axis
])
r = rep()
es = r["entities"]
check("CLOSED", r["buildable"] and r["open_ends"] == [], "the half-profile is one closed loop")
xs = [v[0] for e in es if "p0" in e for v in (e["p0"], e["p1"])]
ys = [v[1] for e in es if "p0" in e for v in (e["p0"], e["p1"])]
check("LENGTH", near(max(xs) - min(xs), LEN), f"overall length exactly {LEN} (the 95 dimension)")
check("VERTEX", near(max(ys), RAD), f"outer radius exactly {RAD} (the dia 27)")
fil = [e for e in es if e["type"] == "arc"][0]
check("ARC", near(fil["radius"], RF), f"the corner fillet is exactly R{RF:g}")
cone = [e for e in es if e["type"] == "line"
        and not near(e["p0"][0], e["p1"][0]) and not near(e["p0"][1], e["p1"][1])
        and e["length"] > 5]
if cone:
    c0 = cone[0]
    a = abs(math.degrees(math.atan2(c0["p1"][1] - c0["p0"][1], c0["p1"][0] - c0["p0"][0])))
    check("ANGLE", near(a, 23, 1e-6), f"the cone is exactly 23 degrees to the axis (got {a:.6f})")
cham = [e for e in es if e["type"] == "line" and near(e["length"], CH * math.sqrt(2), 1e-9)]
check("ANGLE", bool(cham), "the C1 chamfer is exactly 1 x 45 (length 1*sqrt2)")
tang = True
for p in endpoints(fil):
    for m in [e for e in es if e is not fil and any(pt_near(p, q) for q in endpoints(e))]:
        if not smooth(fil, m, p):
            tang = False
check("TANGENT", tang, "the fillet is tangent to BOTH the cylinder and the cone (no kink)")

call("sketch_cancel")

try:
    call("sketch_cancel")
except Exception:
    pass                      # a rung may have closed it already
print(f"\n{'ALL RUNGS HELD' if _fail == 0 else str(_fail) + ' CHECK(S) FAILED'}")
sys.exit(1 if _fail else 0)
