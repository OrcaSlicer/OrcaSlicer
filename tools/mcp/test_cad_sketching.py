#!/usr/bin/env python3
"""Checks that sketches drawn on the Design tab the way a person draws them come out as typed.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_sketching.py [rung ...]

With no rung named, every rung runs. Each one opens a sketch on the XY plane (Shift+S, then a click
on the plane), draws with clicks on the canvas, types the exact values into the canvas's value field
with no click into it, and grades the geometry and loops sketch_describe reads back. It covers what
the CAD tools that build sketches skip: the gesture state, the queue of value fields, snapping,
inference at gesture tolerance and the keyboard reaching the field. The click only has to be roughly
right; the typed number is what must come back exactly. Every action goes through the MCP UI tools,
and call_cad_tool only reads, save the scale rung's one bulk add. Exits 0 when every check holds.

The rungs: each 2D tool draws its primitive and takes its exact values from the keyboard (rect,
circle, line, arc, slot, polygon, ellipse, point, spline), and voids draws one closed outer profile
with two internal voids, all by gesture in one sketch, and checks that the outer loop owns both.
The edit operations (fillet, chamfer, offset, mirror, mirror_arcs, trim, extend) are driven by the
picks and the on-geometry value label a person uses, and dimension puts a typed length on a drawn
shape. The constraint rungs press each of the twenty constraint buttons on drawn geometry, mostly
in Constrain mode (K) and once, live_constrain, in the open sketch, and grade the geometry the
constraint produced and that it survives committing the sketch and opening it again; type_guards
checks that a button that cannot apply stores nothing. badge clicks a constraint's badge, which
selects it and deletes nothing, as does a double-click, then deletes it with Delete. undo and
feature_undo undo inside a sketch and across its commit, from the keyboard. roundtrip saves the project and opens it again through
the File menu, its file pickers answered by auto_answer_dialogs rules with the path (skipped on
Linux, where a GTK picker can't take one). scale draws on top of a sketch holding 1204 entities,
which it loads with one call_cad_tool sketch_add while a tool is armed; the call says why.
selfcheck-recovery runs only when named: it raises with the mouse button held, the offer menu
asked for and a tool armed, to show that the rungs after it start clean.
"""

import math
import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from cad_gui import CANVAS, CadGui, until  # noqa: E402

gui = None  # the CadGui main() starts


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def lengths(ents):
    return sorted(round(e["length"], 6) for e in ents if e["type"] == "line")


def loops():
    return gui.sketch()["closed_loops"]


def key(k):
    """A tool key or Esc, pressed over the canvas, whichever widget a ribbon click left holding the
    keyboard focus."""
    gui.keys(k, target=CANVAS)


# ---------------------------------------------------------------- one tool each

def rung_rect():
    gui.enter_sketch("r")
    gui.click_mm(-60, -40)
    gui.click_mm(60, 40)
    gui.values(120, 80)
    d = gui.sketch()
    ls = lengths(d["entities"])
    gui.check("rect: the sides are the typed 120 x 80", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = d["closed_loops"]
    gui.check("rect: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    gui.check("rect: area 9600", len(lp) == 1 and near(abs(lp[0]["area"]), 9600.0, 1e-6),
              f"areas {[abs(l['area']) for l in lp]}")
    gui.check("rect: every side axis-aligned",
              all(near(abs(e["p1"][0] - e["p0"][0]), 0, 1e-9) or near(abs(e["p1"][1] - e["p0"][1]), 0, 1e-9)
                  for e in d["entities"]), d["entities"])
    gui.cancel()


def rung_circle():
    gui.enter_sketch("c")
    gui.click_mm(0, 0)
    gui.click_mm(30, 0)
    gui.values(25)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "circle"]
    gui.check("circle: radius the typed 25", len(e) == 1 and near(e[0]["radius"], 25.0),
              f"radius {e[0]['radius'] if e else None}")
    gui.check("circle: centre at the clicked origin (0.6 mm)",
              len(e) == 1 and near(e[0]["center"][0], 0.0, 0.6) and near(e[0]["center"][1], 0.0, 0.6),
              f"centre {e[0]['center'] if e else None}")
    lp = d["closed_loops"]
    gui.check("circle: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    gui.check("circle: area pi r^2", len(lp) == 1 and near(abs(lp[0]["area"]), math.pi * 625.0, 1e-6),
              f"areas {[abs(l['area']) for l in lp]} vs {math.pi * 625:.6f}")
    gui.cancel()


def rung_line():
    gui.enter_sketch("l")
    gui.click_mm(-40, -20)
    gui.click_mm(10, 5)
    gui.values(50, 30)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "line"]
    gui.check("line: length the typed 50", len(e) == 1 and near(e[0]["length"], 50.0),
              f"lengths {[x['length'] for x in e]}")
    if e:
        a = math.degrees(math.atan2(e[0]["p1"][1] - e[0]["p0"][1], e[0]["p1"][0] - e[0]["p0"][0])) % 360.0
        gui.check("line: angle the typed 30 deg", near(a, 30.0, 1e-9), f"angle {a:.9f} deg")
    gui.cancel()


def rung_arc():
    gui.enter_sketch("a")
    gui.click_mm(-40, 0)
    gui.click_mm(40, 0)
    gui.click_mm(0, 40)
    gui.values(40, 90)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "arc"]
    gui.check("arc: radius the typed 40", len(e) == 1 and near(e[0]["radius"], 40.0),
              f"radii {[x['radius'] for x in e]}")
    if e:
        sw = abs(e[0]["end_angle"] - e[0]["start_angle"]) * 180.0 / math.pi
        # 1e-7 deg, not exact: the sweep is read back as end_angle - start_angle, two atan2 results,
        # where the line's angle is stored as the direction it was given. 1e-9 deg at r=40 is
        # 7e-10 mm, a float round trip.
        gui.check("arc: sweep the typed 90 deg", near(sw, 90.0, 1e-7), f"sweep {sw:.9f} deg")
        ch = math.dist(e[0]["p0"], e[0]["p1"])
        gui.check("arc: chord r*sqrt2", near(ch, 40.0 * math.sqrt(2.0), 1e-6),
                  f"chord {ch:.6f} vs {40 * math.sqrt(2):.6f}")
    gui.cancel()


def rung_slot():
    gui.enter_sketch("s")
    gui.click_mm(-30, 0)
    gui.click_mm(30, 0)
    gui.click_mm(30, 12)
    # The fields are the centre distance, the slot's full width and its angle: a width of 20 gives
    # the end radius of 10 graded below.
    gui.values(60, 20, 0)
    d = gui.sketch()
    arcs = [x for x in d["entities"] if x["type"] == "arc"]
    lns = [x for x in d["entities"] if x["type"] == "line"]
    gui.check("slot: two end arcs of radius 10, half the typed width", len(arcs) == 2 and all(near(a["radius"], 10.0) for a in arcs),
              f"end radii {[round(a['radius'], 9) for a in arcs]}")
    gui.check("slot: two flanks of the typed centre distance 60",
              len(lns) == 2 and all(near(l["length"], 60.0) for l in lns),
              f"flanks {[round(l['length'], 9) for l in lns]}")
    lp = d["closed_loops"]
    gui.check("slot: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    gui.check("slot: area 60*20 + pi*100", len(lp) == 1 and near(abs(lp[0]["area"]), 60 * 20 + math.pi * 100, 1e-6),
              f"areas {[abs(l['area']) for l in lp]} vs {60 * 20 + math.pi * 100:.6f}")
    gui.cancel()


def rung_polygon():
    gui.enter_sketch("g")
    gui.click_mm(0, 0)
    # A vertex a little off the X axis puts two edges within the axis inference's 3 degrees of
    # horizontal, on every screen whatever pixel the click lands on: the polygon stays regular.
    gui.click_mm(35, 1)
    gui.values(30, 0)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "line"]
    ls = lengths(d["entities"])
    gui.check("polygon: at least 3 sides, every one the typed 30", len(e) >= 3 and all(near(l, 30.0, 1e-9) for l in ls),
              f"{len(e)} sides {sorted(set(ls))}")
    lp = d["closed_loops"]
    gui.check("polygon: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    if e:
        n = len(e)
        want = n * 30.0 ** 2 / (4.0 * math.tan(math.pi / n))
        gui.check(f"polygon: area of a regular {n}-gon of side 30", len(lp) == 1 and near(abs(lp[0]["area"]), want, 1e-6),
                  f"areas {[abs(l['area']) for l in lp]} vs {want:.6f}")
    gui.cancel()


def rung_ellipse():
    gui.enter_sketch("e")
    gui.click_mm(0, 0)
    gui.click_mm(40, 0)
    gui.click_mm(0, 15)
    gui.values(50, 20)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "ellipse"]
    gui.check("ellipse: one ellipse", len(e) == 1, f"{len(e)} ellipses")
    lp = d["closed_loops"]
    gui.check("ellipse: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    gui.check("ellipse: area pi*a*b of the typed 50 x 20",
              len(lp) == 1 and near(abs(lp[0]["area"]), math.pi * 50 * 20, 2e-2),
              f"areas {[round(abs(l['area']), 4) for l in lp]} vs {math.pi * 1000:.4f}")
    gui.cancel()


def rung_point():
    gui.enter_sketch("p")
    gui.click_mm(20, 10)
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "point"]
    gui.check("point: one point where clicked (0.6 mm)",
              len(e) == 1 and near(e[0]["p"][0], 20.0, 0.6) and near(e[0]["p"][1], 10.0, 0.6),
              f"points {[[round(v, 3) for v in x['p']] for x in e]}")
    gui.cancel()


def rung_spline():
    gui.enter_sketch("b")
    for p in [(-40, 0), (-15, 30), (15, -30), (40, 0)]:
        gui.click_mm(*p)
    gui.click_mm(40, 0, button="right")  # a right click ends the spline
    d = gui.sketch()
    e = [x for x in d["entities"] if x["type"] == "spline"]
    gui.check("spline: one spline from 4 control points", len(e) == 1, f"{len(e)} splines")
    gui.cancel()


# ---------------------------------------------------------------- voids by hand

def rung_voids():
    """One closed outer loop with internal voids, every one drawn by gesture in one sketch and given
    its size from the keyboard."""
    gui.enter_sketch("r")
    gui.click_mm(-60, -40)
    gui.click_mm(60, 40)  # outer 120 x 80
    gui.values(120, 80)
    gui.keys("r")  # the same tool again, from the keyboard
    gui.click_mm(-45, -15)
    gui.click_mm(-5, 15)  # void 1: 40 x 30
    gui.values(40, 30)
    gui.keys("c")
    gui.click_mm(30, 0)
    gui.click_mm(42, 0)  # void 2: circle r 10
    gui.values(10)
    d = gui.sketch()
    ls = lengths(d["entities"])
    gui.check("voids: the sides are the typed 120 x 80 and 40 x 30",
              ls == [30.0, 30.0, 40.0, 40.0, 80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = d["closed_loops"]
    gui.check("voids: three closed loops", len(lp) == 3 and all(l["closed"] for l in lp), f"{len(lp)} closed loops")
    gui.check("voids: buildable, nothing dangling", d["buildable"] and not d["open_ends"],
              f"buildable {d['buildable']}, open ends {d['open_ends']}")
    if not lp:
        return
    # The void attribution is the property under test: the outer loop must own both inner ones,
    # and neither inner loop may claim a hole of its own.
    outer = max(range(len(lp)), key=lambda i: abs(lp[i]["area"]))
    holes = sorted(lp[outer]["holes"])
    gui.check("voids: the outer loop owns both voids", holes == sorted(i for i in range(len(lp)) if i != outer),
              f"outer loop {outer} owns holes {holes}")
    gui.check("voids: neither void claims a hole of its own", all(not lp[i]["holes"] for i in range(len(lp)) if i != outer),
              [lp[i]["holes"] for i in range(len(lp)) if i != outer])
    a = {i: abs(lp[i]["area"]) for i in range(len(lp))}
    gui.check("voids: outer area 9600", near(a[outer], 9600.0, 1e-6), f"outer {a[outer]:.6f}")
    inner = sorted(a[i] for i in a if i != outer)
    gui.check("voids: void areas pi*100 and 40*30",
              len(inner) == 2 and near(inner[0], math.pi * 100, 1e-6) and near(inner[1], 1200.0, 1e-6),
              f"voids {[round(v, 6) for v in inner]}")
    net = a[outer] - sum(v for i, v in a.items() if i != outer)
    gui.check("voids: net material 9600 - 1200 - pi*100", near(net, 9600.0 - 1200.0 - math.pi * 100, 1e-6),
              f"net {net:.6f}")
    gui.cancel()


# ---------------------------------------------------------------- combining
# Mirror, offset, trim, extend, fillet and chamfer, each driven by the picks and the on-geometry
# value label a person uses. The label's place is computed from the geometry the tool derives, by
# the tool's own formula (render_op_gizmo), rather than looked for in the pixels.

def op_label_mm(anchor, direction, value, at):
    """Where an operation's value label sits: past the arrow's tip at anchor + dir * value, by 1.2
    label heights, a label height being 15 px in millimetres at `at`."""
    th = max(15 * gui.mm_per_px(*at), 1e-4)
    n = math.hypot(*direction)
    d = (direction[0] / n, direction[1] / n)
    tip = (anchor[0] + d[0] * value, anchor[1] + d[1] * value)
    return (tip[0] + d[0] * th * 1.2, tip[1] + d[1] * th * 1.2)


def mid(e):
    return ((e["p0"][0] + e["p1"][0]) / 2.0, (e["p0"][1] + e["p1"][1]) / 2.0)


def mid_of(a, b):
    return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)


def corner_of(a, b):
    """The shared endpoint of two adjacent lines, and the bisector pointing into their wedge."""
    C = min(((pa, pb) for pa in (a["p0"], a["p1"]) for pb in (b["p0"], b["p1"])),
            key=lambda t: math.dist(t[0], t[1]))[0]

    def away(e):
        f = e["p1"] if math.dist(e["p0"], C) < math.dist(e["p1"], C) else e["p0"]
        n = math.dist(f, C)
        return ((f[0] - C[0]) / n, (f[1] - C[1]) / n)

    ua, ub = away(a), away(b)
    return tuple(C), (ua[0] + ub[0], ua[1] + ub[1])


def corner_pair(ents):
    """Two adjacent lines of a rectangle: the first line and the one sharing an endpoint."""
    ls = [e for e in ents if e["type"] == "line"]
    a = ls[0]
    for b in ls[1:]:
        if min(math.dist(pa, pb) for pa in (a["p0"], a["p1"]) for pb in (b["p0"], b["p1"])) < 1e-6:
            return a, b
    raise AssertionError("no adjacent pair of lines in what should be a rectangle")


def cross(a, b):
    """Where two lines' infinite supports meet."""
    (x1, y1), (x2, y2) = a["p0"], a["p1"]
    (x3, y3), (x4, y4) = b["p0"], b["p1"]
    d = (x2 - x1) * (y4 - y3) - (y2 - y1) * (x4 - x3)
    t = ((x3 - x1) * (y4 - y3) - (y3 - y1) * (x4 - x3)) / d
    return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))


def signed_point_line_dist(p, line):
    """Distance from a point to the infinite line through a segment, signed by the side it is on."""
    (x0, y0), (x1, y1) = line["p0"], line["p1"]
    dx, dy = x1 - x0, y1 - y0
    return (dy * (p[0] - x0) - dx * (p[1] - y0)) / math.hypot(dx, dy)


def point_line_dist(p, line):
    return abs(signed_point_line_dist(p, line))


def draw_rect(w, h, x0, y0):
    gui.click_mm(x0, y0)
    gui.click_mm(x0 + w, y0 + h)
    gui.values(w, h)


def draw_line(x0, y0, x1, y1, length, angle):
    gui.click_mm(x0, y0)
    gui.click_mm(x1, y1)
    gui.values(length, angle)


def rung_fillet():
    gui.enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    a, b = corner_pair(gui.sketch()["entities"])
    key("f")
    gui.click_mm(*mid(a))
    gui.click_mm(*mid(b))
    C, bis = corner_of(a, b)
    gui.click_mm(*op_label_mm(C, bis, 0.2 * min(a["length"], b["length"]), C))
    gui.values(8)
    d = gui.sketch()
    arcs = [x for x in d["entities"] if x["type"] == "arc"]
    gui.check("fillet: one arc of the typed radius 8", len(arcs) == 1 and near(arcs[0]["radius"], 8.0),
              f"radii {[x['radius'] for x in arcs]}")
    lp = d["closed_loops"]
    gui.check("fillet: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    want = 9600.0 - 64.0 * (1.0 - math.pi / 4.0)
    gui.check("fillet: area 9600 - r^2(1 - pi/4)", len(lp) == 1 and near(abs(lp[0]["area"]), want, 1e-6),
              f"areas {[abs(l['area']) for l in lp]} vs {want:.6f}")
    if arcs:
        # Tangent: the arc's centre sits exactly r from each surviving leg's line.
        legs = [x for x in d["entities"] if x["type"] == "line"]
        ds = sorted(point_line_dist(arcs[0]["center"], l) for l in legs)[:2]
        gui.check("fillet: the arc's centre stands off both legs by r", len(ds) == 2 and all(near(x, 8.0, 1e-9) for x in ds),
                  f"stand-offs {ds}")
    gui.cancel()


def rung_chamfer():
    gui.enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    a, b = corner_pair(gui.sketch()["entities"])
    key("h")
    gui.click_mm(*mid(a))
    gui.click_mm(*mid(b))
    C, bis = corner_of(a, b)
    gui.click_mm(*op_label_mm(C, bis, 0.2 * min(a["length"], b["length"]), C))
    gui.values(10)
    d = gui.sketch()
    lp = d["closed_loops"]
    gui.check("chamfer: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    gui.check("chamfer: five sides after the cut", len(lp) == 1 and len(lp[0]["entities"]) == 5,
              f"sides per loop {[len(l['entities']) for l in lp]}")
    ls = sorted(e["length"] for e in d["entities"] if e["type"] == "line")
    gui.check("chamfer: the new face is d*sqrt2", any(near(x, 10.0 * math.sqrt(2.0), 1e-9) for x in ls),
              f"sides {[round(x, 9) for x in ls]} vs {10 * math.sqrt(2):.9f}")
    gui.check("chamfer: both legs shortened by exactly d",
              len(ls) >= 4 and near(ls[1], 70.0, 1e-9) and near(ls[3], 110.0, 1e-9), f"sides {[round(x, 9) for x in ls]}")
    gui.check("chamfer: area 9600 - d^2/2", len(lp) == 1 and near(abs(lp[0]["area"]), 9600.0 - 50.0, 1e-6),
              f"areas {[abs(l['area']) for l in lp]}")
    gui.cancel()


def rung_offset():
    gui.enter_sketch("c")
    gui.click_mm(0, 0)
    gui.click_mm(30, 0)
    gui.values(25)
    c = next(x for x in gui.sketch()["entities"] if x["type"] == "circle")
    key("o")
    gui.click_mm(25, 0)  # pick the rim
    # A circle's offset anchors at centre + (r, 0). A positive distance is to the left of travel,
    # which for a circle is inward, so the arrow points along -x, and it starts at a twentieth of
    # the outline's size.
    anchor = (c["center"][0] + c["radius"], c["center"][1])
    gui.click_mm(*op_label_mm(anchor, (-1.0, 0.0), 0.05 * 2 * c["radius"], anchor))
    gui.values(5)
    d = gui.sketch()
    cs = sorted(x["radius"] for x in d["entities"] if x["type"] == "circle")
    # The typed 5 lands on the arrow's side, inward; what counts is that the separation is exactly
    # the number typed and the picked circle keeps its own radius.
    gui.check("offset: the copy sits exactly the typed 5 inside the circle",
              len(cs) == 2 and near(cs[1], 25.0) and near(cs[1] - cs[0], 5.0), f"radii {cs}")
    lp = d["closed_loops"]
    gui.check("offset: two closed loops", len(lp) == 2 and all(l["closed"] for l in lp), f"{len(lp)} closed loops")
    gui.check("offset: the inner circle is a void of the outer", any(l["holes"] for l in lp),
              f"holes {[l['holes'] for l in lp]}")
    gui.cancel()


def rung_mirror():
    gui.enter_sketch("l")
    gui.press("Construction")  # the axis is reference, not material
    key("l")
    draw_line(0, -40, 0, 40, 80, 90)  # the axis, on x = 0
    gui.press("Construction")  # back to real geometry
    key("l")
    draw_line(0, -40, 50, -40, 50, 0)
    key("l")
    draw_line(50, -40, 50, 40, 80, 90)
    key("l")
    draw_line(50, 40, 0, 40, 50, 180)
    d0 = gui.sketch()["entities"]
    axis = [e for e in d0 if e.get("construction")]
    half = [e for e in d0 if e["type"] == "line" and not e.get("construction")]
    gui.check("mirror: one construction axis", len(axis) == 1, f"{len(axis)} construction entities")
    gui.check("mirror: three lines in the half profile", len(half) == 3, f"{len(half)} real lines")
    if not axis or not half:
        return  # nothing to mirror, or nothing to mirror about
    key("m")
    gui.click_mm(*mid(axis[0]))
    for e in half:
        gui.click_mm(*mid(e))
    key("Return")  # applies the mirror the picks set up; a click on empty space leaves it pending
    d = gui.sketch()
    real = [e for e in d["entities"] if e["type"] == "line" and not e.get("construction")]
    gui.check("mirror: six lines after the reflection", len(real) == 6, f"{len(real)} real lines")
    lp = d["closed_loops"]
    gui.check("mirror: one closed loop", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    # Graded against the geometry drawn, not the coordinates aimed at: a click lands on a whole
    # pixel, so the half profile sits a few tenths of a millimetre off the origin. The typed values
    # fix its lengths and angles, not its anchor, and the mirror is what is under test.
    far = max(half, key=lambda e: e["length"])  # the edge parallel to the axis
    w = point_line_dist(mid(far), axis[0])
    want = 2.0 * w * far["length"]
    gui.check("mirror: area twice the half profile's", len(lp) == 1 and near(abs(lp[0]["area"]), want, 1e-6),
              f"areas {[abs(l['area']) for l in lp]} vs 2 x {w:.6f} x {far['length']:.6f} = {want:.6f}")
    # Symmetry is what the rung exists for: every vertex has its exact reflection about the axis
    # that was drawn.
    (ax, ay), (bx, by) = axis[0]["p0"], axis[0]["p1"]

    def refl(q):
        dx, dy = bx - ax, by - ay
        t = ((q[0] - ax) * dx + (q[1] - ay) * dy) / (dx * dx + dy * dy)
        fx, fy = ax + t * dx, ay + t * dy
        return (2 * fx - q[0], 2 * fy - q[1])

    vs = [tuple(p) for e in real for p in (e["p0"], e["p1"])]
    missing = [v for v in vs if not any(math.dist(refl(v), o) < 1e-9 for o in vs)]
    gui.check("mirror: every vertex has its exact reflection about the drawn axis", not missing,
              f"{len(missing)} of {len(vs)} vertices without one: {missing}")
    gui.cancel()


def rung_mirror_arcs():
    """Mirror a shape that has arcs in it: a slot. The mirror rung reflects straight lines only.
    An arc has five degrees of freedom, so a copy bound to its source by its centre alone leaves
    the solver free to answer with another sketch that is consistent in itself: caps grown and
    swept past a half turn, a rail collapsed, and the source wrecked along with the copy. A circle
    has no endpoints to leave free, so circles mirror correctly either way. Graded on what a person
    expects: the applied result is the preview, every copy is its source reflected, the sources do
    not move, and no copied cap comes back reflex."""
    gui.enter_sketch("l")
    gui.press("Construction")  # the axis is reference, not material
    key("l")
    draw_line(0, -40, 0, 40, 80, 90)  # the axis, on x = 0
    gui.press("Construction")  # back to real geometry
    key("s")  # the slot: the centreline's two ends, then its width
    gui.click_mm(-70, -10)
    gui.click_mm(-30, -10)
    gui.click_mm(-30, 0)
    # Typed, so the slot is exact before it is mirrored. The fields are the centre distance, the
    # slot's full width and its angle: a width of 20 gives caps of radius 10.
    gui.values(40, 20, 0)
    d0 = gui.sketch()["entities"]
    axes = [e for e in d0 if e.get("construction")]
    if len(axes) != 1:
        raise AssertionError(f"{len(axes)} construction entities where the one axis was drawn")
    axis = axes[0]
    slot = [e for e in d0 if not e.get("construction")]
    arcs0 = [e for e in slot if e["type"] == "arc"]
    gui.check("mirror_arcs: two caps on the slot", len(arcs0) == 2, f"{len(arcs0)} caps")
    key("m")
    gui.click_mm(*mid(axis))
    for e in slot:
        if e["type"] == "line":
            gui.click_mm(*mid(e))
        else:  # a point on the arc, at the middle of its sweep
            a = (e["start_angle"] + e["end_angle"]) / 2.0
            gui.click_mm(e["center"][0] + e["radius"] * math.cos(a), e["center"][1] + e["radius"] * math.sin(a))
    key("Return")  # applies the mirror the picks set up; a click on empty space leaves it pending
    d1 = gui.sketch()["entities"]
    gui.check("mirror_arcs: one copy for each picked entity", len(d1) == len(d0) + len(slot),
              f"{len(d1) - len(d0)} copies for {len(slot)} picked entities")

    def shape_of(e):
        if e["type"] == "arc":
            return (round(e["radius"], 9), round(abs(e["end_angle"] - e["start_angle"]), 9))
        return (round(math.dist(e["p0"], e["p1"]), 9),)

    # The sources, entity by entity, keep their shape.
    moved = [i for i, e in enumerate(d0) if i >= len(d1) or shape_of(e) != shape_of(d1[i])]
    gui.check("mirror_arcs: the mirror left every source alone", not moved, f"moved: {moved}")
    # Every copy is its source reflected. The endpoints are compared either way round, since a
    # reflection reverses orientation and may store p0 and p1 swapped.
    (ax, ay), (bx, by) = axis["p0"], axis["p1"]
    dx, dy = bx - ax, by - ay
    n = math.hypot(dx, dy)
    dx, dy = dx / n, dy / n

    def refl(q):
        vx, vy = q[0] - ax, q[1] - ay
        k = 2.0 * (vx * dx + vy * dy)
        return (ax + k * dx - vx, ay + k * dy - vy)

    copies = d1[len(d0):]
    worst = 0.0
    for e in slot:
        best = min((max(min(max(math.dist(refl(e["p0"]), c["p0"]), math.dist(refl(e["p1"]), c["p1"])),
                            max(math.dist(refl(e["p0"]), c["p1"]), math.dist(refl(e["p1"]), c["p0"]))),
                        abs(shape_of(e)[0] - shape_of(c)[0]))
                    for c in copies if c["type"] == e["type"]), default=math.inf)
        worst = max(worst, best)
    gui.check("mirror_arcs: every copy is its source's exact reflection", worst <= 1e-6, f"worst {worst:.9f}")
    reflex = [c for c in copies if c["type"] == "arc" and abs(c["end_angle"] - c["start_angle"]) > math.pi + 1e-9]
    gui.check("mirror_arcs: no copied cap came back reflex", not reflex,
              f"{len(reflex)} reflex cap(s): sweeps {[abs(c['end_angle'] - c['start_angle']) for c in reflex]}")
    gui.cancel()


def rung_trim():
    gui.enter_sketch("l")
    draw_line(-50, 0, 50, 0, 100, 0)
    key("l")
    draw_line(0, -50, 0, 50, 100, 90)
    d0 = gui.sketch()["entities"]
    horiz = min(d0, key=lambda e: abs(e["p1"][1] - e["p0"][1]))
    vert = max(d0, key=lambda e: abs(e["p1"][1] - e["p0"][1]))
    X = cross(horiz, vert)
    left = min(horiz["p0"], horiz["p1"])  # the end that must survive
    want = math.dist(left, X)
    key("t")
    gui.click_mm(*mid_of(X, max(horiz["p0"], horiz["p1"])))  # the arm on the far side of the crossing
    d = gui.sketch()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    gui.check("trim: the picked arm is gone at the crossing, the other line untouched",
              len(ls) == 2 and near(ls[1], vert["length"], 1e-9) and near(ls[0], want, 1e-9),
              f"lengths {ls}, expected {want:.9f} and {vert['length']:.9f}")
    ends = [tuple(p) for e in d["entities"] if e["type"] == "line" for p in (e["p0"], e["p1"])]
    gui.check("trim: the cut lands exactly on the crossing", any(math.dist(X, q) < 1e-9 for q in ends),
              f"crossing {X}, ends {ends}")
    gui.cancel()


def rung_extend():
    gui.enter_sketch("l")
    draw_line(-50, 0, -10, 0, 40, 0)
    key("l")
    draw_line(0, -50, 0, 50, 100, 90)
    d0 = gui.sketch()["entities"]
    short = min(d0, key=lambda e: e["length"])
    vert = max(d0, key=lambda e: e["length"])
    X = cross(short, vert)
    far = min((short["p0"], short["p1"]), key=lambda q: q[0])  # the end that stays put
    near_end = max((short["p0"], short["p1"]), key=lambda q: q[0])
    want = math.dist(far, X)
    key("x")
    gui.click_mm(*mid_of(near_end, mid_of(far, near_end)))  # click the end that must grow
    d = gui.sketch()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    gui.check("extend: the short line grew to exactly the target line", len(ls) == 2 and near(ls[0], want, 1e-9),
              f"lengths {ls}: {short['length']:.6f} should be {want:.9f}")
    ends = [tuple(p) for e in d["entities"] if e["type"] == "line" for p in (e["p0"], e["p1"])]
    gui.check("extend: the new end sits exactly on the target line", any(math.dist(X, q) < 1e-9 for q in ends),
              f"crossing {X}, ends {ends}")
    gui.cancel()


# ---------------------------------------------------------------- dimensions and constraints
# A drawn shape with no numbers on it, then numbers put on it by hand: the Dimension tool for a
# value, the constraint buttons for a relation. Both must hold the value they were given and take
# the degrees of freedom away: a dimension that moves the geometry but leaves the DoF standing has
# not constrained anything, it has only nudged it.

def escape_field():
    """Esc on the value field the last gesture opened, which closes it and the fields queued after
    it and keeps the shape as drawn."""
    if until(gui.value_field) is None:
        raise AssertionError("the gesture opened no value field")
    key("Escape")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError("Esc left the value field open")


def keep_as_drawn():
    """Esc twice after a drawn shape: the first closes its value fields, the second drops the tool
    to Select."""
    escape_field()
    key("Escape")
    if not until(lambda: gui.sketch()["tool"] == "select"):
        raise AssertionError(f"Esc left the tool {gui.sketch()['tool']} armed")


def draw_rect_undimensioned():
    """A rectangle by two clicks, its value fields dismissed so it keeps its drawn size."""
    gui.click_mm(-60, -40)
    gui.click_mm(60, 40)
    keep_as_drawn()


def accept_shown_value():
    """Return on the value field a button opened, with nothing typed: it commits the value the
    field opened with."""
    field = until(gui.value_field)
    if field is None:
        raise AssertionError("the button opened no value field")
    gui.keys("Return")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError(f"Return left the {field.title} field open")


def pick(rung, *points):
    """Click each point in Constrain mode, and check that every click picked an entity of its own.

    A click the plane map put off its entity picks nothing, and the button pressed next acts on
    fewer entities than the rung means, or on none, which a rung whose checks are all negative
    would not notice. The panel's status line counts the picks. A click on empty space drops them
    all and one on an entity already picked starts them again from it, so the count reaches one
    per click only when every click landed on a different entity. A click that re-picks the one
    entity a press left picked keeps the count, and the line goes on showing what the press said,
    which picked() reads as None."""
    counts = []
    for n, p in enumerate(points, 1):
        gui.click_mm(*p)
        until(lambda: gui.picked() == n, timeout=1.0)
        counts.append(gui.picked())
    n = len(points)
    what = {1: "the pick", 2: "both picks", 3: "all three picks"}[n]
    gui.check(f"{rung}: {what} landed on an entity",
              counts[-1] == n and all(c in (i, None) for i, c in enumerate(counts, 1)),
              f"picked after each click: {counts}")


def confirm_and_reopen():
    """Leave Constrain mode (or the sketch) with the ribbon's Confirm, then open the sketch again
    from its Feature tree row; returns sketch_describe.

    The degrees of freedom and the constraint count have to be read here, not in Constrain mode:
    while constraining, sketch_describe reports the live tool's, which the constrain session does
    not touch, since it works on the committed feature's own constraints. Read after the round
    trip, a constraint that landed shows, and is shown to have been kept rather than previewed."""
    gui.confirm()
    return gui.reopen()


def leave():
    """Leave the sketch, discarding what it holds, and empty the design: a constraint rung commits
    its sketch, and the rungs after it start from no feature."""
    gui.cancel()
    gui.new_design()


def angle_between(a, b):
    va = (a["p1"][0] - a["p0"][0], a["p1"][1] - a["p0"][1])
    vb = (b["p1"][0] - b["p0"][0], b["p1"][1] - b["p0"][1])
    c = (va[0] * vb[0] + va[1] * vb[1]) / (math.hypot(*va) * math.hypot(*vb))
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def parallel_gap(a, b):
    """How far from parallel, in degrees. Parallel reads 0 or 180; both mean parallel."""
    ang = angle_between(a, b)
    return min(ang, 180.0 - ang)


def rim(e, deg=135.0):
    """A point on a circle's rim, away from +X: a click on the +X point grabs the radius grip
    instead of picking the circle."""
    a = math.radians(deg)
    return (e["center"][0] + e["radius"] * math.cos(a), e["center"][1] + e["radius"] * math.sin(a))


def survived(rung, d2, more_than=0):
    """The check every constraint rung ends with: the constraints were kept through the commit."""
    gui.check(f"{rung}: the constraints survived the commit", d2["solve_ok"] and d2["constraints"] > more_than,
              f"{d2['constraints']} constraints, solve_ok {d2['solve_ok']}, dof {d2['dof']}")


def two_lines():
    """Two free lines joined at one end, 56 degrees apart: well outside inference's snap to
    parallel or perpendicular, so the button under test has the work to do."""
    gui.click_mm(-50, -30)
    gui.click_mm(30, -18)
    keep_as_drawn()
    key("l")
    gui.click_mm(30, -18)
    gui.click_mm(55, 35)
    keep_as_drawn()


def two_circles():
    """Two free circles of different sizes, their radius fields dismissed."""
    gui.click_mm(-35, 0)
    gui.click_mm(-20, 0)
    escape_field()
    key("c")
    gui.click_mm(35, 0)
    gui.click_mm(60, 0)
    escape_field()


def rung_dimension():
    gui.enter_sketch("r")
    draw_rect_undimensioned()
    d0 = gui.sketch()
    dof0 = d0["dof"]
    gui.check("dimension: the undimensioned rectangle has degrees of freedom", dof0 > 0, f"dof {dof0}")
    side = max((e for e in d0["entities"] if e["type"] == "line"), key=lambda e: e["length"])
    key("d")
    gui.click_mm(*mid(side))
    gui.values(90)
    d = gui.sketch()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    gui.check("dimension: a side reads the typed 90", any(near(x, 90.0, 1e-9) for x in ls), f"sides {ls}")
    gui.check("dimension: the degrees of freedom went down", d["dof"] < dof0, f"dof {dof0} -> {d['dof']}")
    gui.check("dimension: still one closed, solved loop", d["solve_ok"] and len(d["closed_loops"]) == 1,
              f"solve_ok {d['solve_ok']}, {len(d['closed_loops'])} closed loop(s)")
    gui.cancel()


def rung_constrain():
    """Equal length on two adjacent sides of a rectangle, in Constrain mode."""
    gui.new_design()
    gui.enter_sketch("r")
    draw_rect_undimensioned()
    a, b = corner_pair(gui.sketch()["entities"])
    gui.check("constrain: the two sides start unequal", not near(a["length"], b["length"], 1e-6),
              f"{a['length']:.6f} vs {b['length']:.6f}")
    # The picks are read again from the committed sketch: committing repackages the entities, so
    # an index taken before Constrain is not the same index afterwards.
    d1 = gui.constrain()
    a, b = corner_pair(d1["entities"])
    dof0 = 4  # an undimensioned rectangle: position and size
    ia, ib = d1["entities"].index(a), d1["entities"].index(b)
    pick("constrain", mid(a), mid(b))
    gui.press("Equal length")
    d = gui.sketch()
    la, lb = d["entities"][ia]["length"], d["entities"][ib]["length"]
    gui.check("constrain: the two sides are now equal", near(la, lb, 1e-9), f"{la:.9f} and {lb:.9f}")
    d2 = confirm_and_reopen()
    gui.check("constrain: one degree of freedom fewer after the round trip", d2["dof"] == dof0 - 1,
              f"dof {dof0} -> {d2['dof']}")
    survived("constrain", d2)
    ls2 = sorted(round(e["length"], 9) for e in d2["entities"] if e["type"] == "line")
    gui.check("constrain: the geometry came back unchanged", bool(ls2) and near(ls2[0], la, 1e-9) and near(ls2[-1], la, 1e-9),
              f"sides {ls2} vs {la:.9f}")
    leave()


def rung_perpendicular():
    gui.new_design()
    gui.enter_sketch("l")
    two_lines()
    d0 = gui.sketch()
    a0 = angle_between(d0["entities"][0], d0["entities"][1])
    gui.check("perpendicular: the lines start off square", abs(a0 - 90.0) > 1e-3, f"{a0:.6f} deg")
    d1 = gui.constrain()
    pick("perpendicular", mid(d1["entities"][0]), mid(d1["entities"][1]))
    gui.press("Perpendicular")
    d = gui.sketch()
    ang = angle_between(d["entities"][0], d["entities"][1])
    # 1e-6 deg (1.7e-8 rad): the live solve converges to its own tolerance from a 56 deg start.
    # The round trip below demands exactly 90: the committed feature re-solves from scratch.
    gui.check("perpendicular: the lines are now at 90 deg", near(ang, 90.0, 1e-6), f"{ang:.9f} deg")
    d2 = confirm_and_reopen()
    ang2 = angle_between(d2["entities"][0], d2["entities"][1])
    gui.check("perpendicular: still exactly 90 deg after the round trip", near(ang2, 90.0, 1e-9), f"{ang2:.9f} deg")
    survived("perpendicular", d2)
    leave()


def rung_parallel():
    gui.new_design()
    gui.enter_sketch("l")
    two_lines()
    d0 = gui.sketch()
    g0 = parallel_gap(d0["entities"][0], d0["entities"][1])
    gui.check("parallel: the lines start off parallel", g0 > 1e-3, f"{g0:.6f} deg from parallel")
    d1 = gui.constrain()
    pick("parallel", mid(d1["entities"][0]), mid(d1["entities"][1]))
    gui.press("Parallel")
    d = gui.sketch()
    g = parallel_gap(d["entities"][0], d["entities"][1])
    gui.check("parallel: the lines are now parallel", near(g, 0.0, 1e-6), f"{g:.9f} deg from parallel")
    la, lb = d["entities"][0]["length"], d["entities"][1]["length"]
    gui.check("parallel: neither line collapsed", la > 1.0 and lb > 1.0, f"lengths {la:.6f}, {lb:.6f}")
    survived("parallel", confirm_and_reopen())
    leave()


def rung_live_constrain():
    """A constraint applied in the open sketch: no K, no commit, no tree pick."""
    gui.new_design()
    gui.enter_sketch("l")
    two_lines()
    d0 = gui.sketch()
    gui.check("live_constrain: two entities in the open sketch", len(d0["entities"]) == 2,
              f"{len(d0['entities'])} entities")
    g0 = parallel_gap(d0["entities"][0], d0["entities"][1])
    gui.check("live_constrain: the lines start off parallel", g0 > 1e-3, f"{g0:.6f} deg from parallel")
    # Both picked in the open sketch, the second with Ctrl so it adds to the first rather than
    # replacing it, then the button: no K, which is the step this rung shows is not needed.
    gui.click_mm(*mid(d0["entities"][0]))
    gui.click_mm(*mid(d0["entities"][1]), modifiers=["ctrl"])
    sel, want = sorted(gui.sketch()["selection"]), sorted(e["index"] for e in d0["entities"][:2])
    gui.check("live_constrain: both picks landed, the second adding to the first", sel == want,
              f"selection {sel}, lines {want}")
    gui.press("Parallel")
    d = gui.sketch()
    g = parallel_gap(d["entities"][0], d["entities"][1])
    gui.check("live_constrain: parallel without leaving the sketch", near(g, 0.0, 1e-6), f"{g:.9f} deg from parallel")
    la, lb = d["entities"][0]["length"], d["entities"][1]["length"]
    gui.check("live_constrain: neither line collapsed", la > 1.0 and lb > 1.0, f"lengths {la:.6f}, {lb:.6f}")
    survived("live_constrain", confirm_and_reopen())
    leave()


def rung_badge():
    """A click on a constraint badge selects the constraint and deletes nothing, and Delete then
    removes that one constraint. A badge is small and sits where a person aims at the geometry, so a
    click that deleted would lose a constraint to a slip of the pointer. The badge clicked is the
    Horizontal of the rectangle's first side, found by its place in that side's stack."""
    gui.new_design()
    gui.enter_sketch("r")
    draw_rect_undimensioned()
    side = gui.sketch()["entities"][0]
    cons = gui.cad("sketch_constraints")["items"]
    on_side = [c for c in cons if c["refs"] and c["refs"][0].get("entity") == side["index"]]
    horizontal = next((c for c in on_side if c["type"] == "horizontal"), None)
    if horizontal is None:
        raise AssertionError(f"the rectangle's first side has no Horizontal: {on_side}")
    gui.click_mm(*gui.badge(side, on_side.index(horizontal)))
    d = gui.sketch()
    gui.check("badge: the click deleted no constraint", d["constraints"] == len(cons),
              f"{len(cons)} constraints before the click, {d['constraints']} after")
    gui.check("badge: the click selected the side's Horizontal", d["selected_constraint"] == horizontal["index"],
              f"selected constraint {d['selected_constraint']}, the Horizontal is {horizontal['index']}")
    # A double-click is a press and then a double-click event: the badge takes both, so nothing under
    # it, the sketch's face here, is picked with the second.
    gui.click_mm(*gui.badge(side, on_side.index(horizontal)), double=True)
    d = gui.sketch()
    gui.check("badge: a double-click keeps only the Horizontal selected",
              d["selected_constraint"] == horizontal["index"] and not d["selection"],
              f"selected constraint {d['selected_constraint']}, picked entities {d['selection']}")
    key("Delete")
    left = gui.cad("sketch_constraints")["items"]
    gui.check("badge: Delete removed one constraint", len(left) == len(cons) - 1,
              f"{len(cons)} constraints before Delete, {len(left)} after")
    gui.check("badge: the one removed is the side's Horizontal",
              not any(c["type"] == "horizontal" and c["refs"][0].get("entity") == side["index"] for c in left),
              f"constraints left: {[c['type'] for c in left]}")
    gui.check("badge: nothing stays selected after Delete", gui.sketch()["selected_constraint"] is None)
    leave()


def rung_vertical():
    gui.new_design()
    gui.enter_sketch("l")
    gui.click_mm(-20, -35)
    gui.click_mm(5, 30)
    keep_as_drawn()
    d0 = gui.sketch()
    dx0 = abs(d0["entities"][0]["p1"][0] - d0["entities"][0]["p0"][0])
    gui.check("vertical: the line starts off plumb", dx0 > 1.0, f"dx {dx0:.6f}")
    d1 = gui.constrain()
    pick("vertical", mid(d1["entities"][0]))
    gui.press("Vertical")
    e = gui.sketch()["entities"][0]
    dx = abs(e["p1"][0] - e["p0"][0])
    gui.check("vertical: the line is now plumb", near(dx, 0.0, 1e-6), f"dx {dx:.9f}")
    gui.check("vertical: the line did not collapse", e["length"] > 1.0, f"length {e['length']:.6f}")
    survived("vertical", confirm_and_reopen())
    leave()


def rung_equal_radius_button():
    """The Equal radius button itself; equal_radius covers the shared Equal length button on
    circles."""
    gui.new_design()
    gui.enter_sketch("c")
    two_circles()
    cs = [e for e in gui.sketch()["entities"] if e["type"] == "circle"]
    gui.check("equal_radius_button: the circles start unequal", len(cs) == 2 and not near(cs[0]["radius"], cs[1]["radius"], 1e-6),
              f"radii {[round(c['radius'], 6) for c in cs]}")
    d1 = gui.constrain()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    pick("equal_radius_button", rim(cs[0]), rim(cs[1]))
    gui.press("Equal radius")
    d = gui.sketch()
    ra, rb = d["entities"][ia]["radius"], d["entities"][ib]["radius"]
    gui.check("equal_radius_button: the radii are now equal", near(ra, rb, 1e-9), f"{ra:.9f} and {rb:.9f}")
    gui.check("equal_radius_button: and not equal at zero", ra > 1e-6, f"radius {ra:.9f}")
    leave()


def rung_concentric():
    gui.new_design()
    gui.enter_sketch("c")
    gui.click_mm(-30, -10)
    gui.click_mm(-14, -10)
    escape_field()
    key("c")
    gui.click_mm(28, 14)
    gui.click_mm(52, 14)
    escape_field()
    cs = [e for e in gui.sketch()["entities"] if e["type"] == "circle"]
    gap0 = math.dist(cs[0]["center"], cs[1]["center"])
    r_before = sorted(round(c["radius"], 9) for c in cs)
    gui.check("concentric: the centres start apart", gap0 > 1.0, f"{gap0:.6f} mm apart")
    d1 = gui.constrain()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    pick("concentric", rim(cs[0]), rim(cs[1]))
    gui.press("Concentric")
    d = gui.sketch()
    gap = math.dist(d["entities"][ia]["center"], d["entities"][ib]["center"])
    gui.check("concentric: the centres now coincide", near(gap, 0.0, 1e-6), f"{gap:.9f} mm apart")
    # Concentric is about centres only: a solve that also equalised the radii would be wrong.
    r_after = sorted(round(d["entities"][i]["radius"], 9) for i in (ia, ib))
    gui.check("concentric: the radii are untouched", all(near(a, b, 1e-6) for a, b in zip(r_before, r_after)),
              f"radii {r_before} -> {r_after}")
    survived("concentric", confirm_and_reopen())
    leave()


def rung_tangent():
    gui.new_design()
    gui.enter_sketch("c")
    gui.click_mm(0, 0)
    gui.click_mm(20, 0)
    escape_field()
    key("l")
    # A line that clearly misses the circle, so tangency has to close the gap.
    gui.click_mm(-45, 38)
    gui.click_mm(45, 44)
    keep_as_drawn()
    d0 = gui.sketch()
    c0 = next(e for e in d0["entities"] if e["type"] == "circle")
    l0 = next(e for e in d0["entities"] if e["type"] == "line")
    d_before = point_line_dist(c0["center"], l0)
    gui.check("tangent: the line starts clear of the circle", d_before > c0["radius"] + 1.0,
              f"it misses by {d_before - c0['radius']:.6f} mm")
    d1 = gui.constrain()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    l1 = next(e for e in d1["entities"] if e["type"] == "line")
    ic, il = d1["entities"].index(c1), d1["entities"].index(l1)
    pick("tangent", rim(c1), mid(l1))
    gui.press("Tangent")
    d = gui.sketch()
    cc, ll = d["entities"][ic], d["entities"][il]
    dist = point_line_dist(cc["center"], ll)
    gui.check("tangent: the line touches the circle", near(dist, cc["radius"], 1e-6),
              f"centre to line {dist:.9f}, radius {cc['radius']:.9f}")
    gui.check("tangent: the circle did not collapse", cc["radius"] > 1e-6, f"radius {cc['radius']:.9f}")
    survived("tangent", confirm_and_reopen())
    leave()


def rung_midpoint():
    gui.new_design()
    gui.enter_sketch("l")
    gui.click_mm(-50, -20)
    gui.click_mm(40, 26)
    keep_as_drawn()
    key("p")
    gui.click_mm(10, -35)
    d0 = gui.sketch()
    l0 = next(e for e in d0["entities"] if e["type"] == "line")
    p0 = next(e for e in d0["entities"] if e["type"] == "point")
    off0 = math.dist(p0["p"], mid(l0))
    gui.check("midpoint: the point starts off the line's middle", off0 > 1.0, f"{off0:.6f} mm off")
    d1 = gui.constrain()
    l1 = next(e for e in d1["entities"] if e["type"] == "line")
    p1 = next(e for e in d1["entities"] if e["type"] == "point")
    il, ip = d1["entities"].index(l1), d1["entities"].index(p1)
    # The point first, then the line: Midpoint binds a point on the first pick and a whole segment
    # on the second.
    pick("midpoint", p1["p"], mid(l1))
    gui.press("Midpoint")
    d = gui.sketch()
    ll, pp = d["entities"][il], d["entities"][ip]
    off = math.dist(pp["p"], mid(ll))
    gui.check("midpoint: the point sits on the line's middle", near(off, 0.0, 1e-6), f"{off:.9f} mm off")
    gui.check("midpoint: the line did not collapse", ll["length"] > 1.0, f"length {ll['length']:.6f}")
    survived("midpoint", confirm_and_reopen())
    leave()


def rung_symmetric_line_axis():
    """Symmetric about a picked axis line: two entities and the axis, the only button that takes a
    third pick."""
    gui.new_design()
    gui.enter_sketch("l")
    gui.click_mm(0, -40)
    gui.click_mm(0, 40)  # the axis: a real line on x = 0
    keep_as_drawn()
    key("p")
    gui.click_mm(-38, 12)
    key("p")
    gui.click_mm(14, 12)
    ps = [e for e in gui.sketch()["entities"] if e["type"] == "point"]
    gui.check("symmetric_line_axis: the points start unmirrored", not near(abs(ps[0]["p"][0]), abs(ps[1]["p"][0]), 1e-3),
              f"x {ps[0]['p'][0]:.6f}, {ps[1]['p'][0]:.6f}")
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ax = next(e for e in d1["entities"] if e["type"] == "line")
    ia, ib, iax = d1["entities"].index(ps[0]), d1["entities"].index(ps[1]), d1["entities"].index(ax)
    pick("symmetric_line_axis", ps[0]["p"], ps[1]["p"], mid(ax))
    gui.press("Symmetric")
    d = gui.sketch()
    # Measured against the axis where it is now, not against x = 0: all three entities are free,
    # so the solver may satisfy the mirror by moving the axis instead of the points.
    ax_now = d["entities"][iax]
    sa = signed_point_line_dist(d["entities"][ia]["p"], ax_now)
    sb = signed_point_line_dist(d["entities"][ib]["p"], ax_now)
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    gui.check("symmetric_line_axis: mirrored about the picked line", near(sa, -sb, 1e-6),
              f"{sa:.9f} and {sb:.9f} from it")
    gui.check("symmetric_line_axis: and not both collapsed onto it", abs(sa) > 1e-6, f"|d| {abs(sa):.9f}")
    gui.check("symmetric_line_axis: y untouched on both", near(ya, yb, 1e-6), f"y {ya:.6f}, {yb:.6f}")
    survived("symmetric_line_axis", confirm_and_reopen())
    leave()


def rung_symmetric_h():
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(20, -34)
    key("p")
    gui.click_mm(20, 9)
    ps = [e for e in gui.sketch()["entities"] if e["type"] == "point"]
    gui.check("symmetric_h: the points start unmirrored", not near(abs(ps[0]["p"][1]), abs(ps[1]["p"][1]), 1e-3),
              f"y {ps[0]['p'][1]:.6f}, {ps[1]['p'][1]:.6f}")
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    pick("symmetric_h", ps[0]["p"], ps[1]["p"])
    gui.press("Symmetric about the horizontal axis")
    d = gui.sketch()
    xa, xb = d["entities"][ia]["p"][0], d["entities"][ib]["p"][0]
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    gui.check("symmetric_h: mirrored across y = 0", near(ya, -yb, 1e-9), f"y {ya:.9f} and {yb:.9f}")
    gui.check("symmetric_h: and not both collapsed onto the axis", abs(ya) > 1e-6, f"|y| {abs(ya):.9f}")
    gui.check("symmetric_h: x untouched on both", near(xa, xb, 1e-6), f"x {xa:.6f}, {xb:.6f}")
    survived("symmetric_h", confirm_and_reopen())
    leave()


def rung_radius():
    gui.new_design()
    gui.enter_sketch("c")
    gui.click_mm(0, 0)
    gui.click_mm(31, 0)
    escape_field()
    c0 = next(e for e in gui.sketch()["entities"] if e["type"] == "circle")
    gui.check("radius: the circle starts at another radius", not near(c0["radius"], 22.0, 1e-3), f"radius {c0['radius']:.6f}")
    d1 = gui.constrain()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    ic = d1["entities"].index(c1)
    ctr = list(c1["center"])
    pick("radius", rim(c1))
    gui.press("Radius")
    gui.values(22)
    cc = gui.sketch()["entities"][ic]
    gui.check("radius: the radius is the typed 22", near(cc["radius"], 22.0, 1e-6), f"radius {cc['radius']:.9f}")
    # A Radius constraint moves the rim, never the centre.
    moved = math.dist(cc["center"], ctr)
    gui.check("radius: the centre stayed put", near(moved, 0.0, 1e-6), f"moved {moved:.9f}")
    survived("radius", confirm_and_reopen())
    leave()


def rung_diameter():
    """A typed Diameter is a diameter, not a radius: Diameter wired to the Radius handler would give
    r = 30 for a typed 30, and nothing about the sketch would look wrong."""
    gui.new_design()
    gui.enter_sketch("c")
    gui.click_mm(0, 0)
    gui.click_mm(19, 0)
    escape_field()
    d1 = gui.constrain()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    ic = d1["entities"].index(c1)
    pick("diameter", rim(c1))
    gui.press("Diameter")
    gui.values(30)
    r = gui.sketch()["entities"][ic]["radius"]
    gui.check("diameter: a typed diameter of 30 gives radius 15", near(r, 15.0, 1e-6), f"radius {r:.9f}")
    survived("diameter", confirm_and_reopen())
    leave()


def rung_fix():
    """Fix anchors a point, and the anchor is what the solver moves around. Fix alone is
    unfalsifiable, since nothing moves; a second constraint that would otherwise move the fixed
    point shows which end travels."""
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-25, 0)
    key("p")
    gui.click_mm(15, 0)
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    anchor = list(ps[0]["p"])
    pick("fix", ps[0]["p"])
    gui.press("Fix point (anchor in place)")
    d = gui.sketch()
    gui.check("fix: the fixed point did not move when it was fixed", near(math.dist(d["entities"][ia]["p"], anchor), 0.0, 1e-9),
              f"moved {math.dist(d['entities'][ia]['p'], anchor):.9f}")
    # A horizontal gap far from the current one: only the free point may travel.
    pick("fix", d["entities"][ia]["p"], d["entities"][ib]["p"])
    gui.press("Horizontal distance")
    gui.values(70)
    d = gui.sketch()
    moved_a = math.dist(d["entities"][ia]["p"], anchor)
    gap = abs(d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0])
    gui.check("fix: the gap is the typed 70", near(gap, 70.0, 1e-6), f"gap {gap:.9f}")
    gui.check("fix: the anchored point held station", near(moved_a, 0.0, 1e-6), f"moved {moved_a:.9f}")
    survived("fix", confirm_and_reopen(), more_than=1)
    leave()


def rung_distance_y():
    """Vertical distance, which is not the straight-line one either."""
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-22, -18)
    key("p")
    gui.click_mm(19, 24)
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    dx_before = d1["entities"][ib]["p"][0] - d1["entities"][ia]["p"][0]
    pick("distance_y", ps[0]["p"], ps[1]["p"])
    gui.press("Vertical distance")
    gui.values(35)
    d = gui.sketch()
    gy = abs(d["entities"][ib]["p"][1] - d["entities"][ia]["p"][1])
    gx = d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0]
    gui.check("distance_y: the vertical gap is the typed 35", near(gy, 35.0, 1e-6), f"gap {gy:.9f}")
    gui.check("distance_y: the horizontal gap is untouched", near(gx, dx_before, 1e-6), f"{dx_before:.9f} -> {gx:.9f}")
    survived("distance_y", confirm_and_reopen())
    leave()


def rung_equal_radius():
    """The shared Equal length button on two circles means equal radius. The circles carry no
    typed radius: a typed radius is a driving Radius constraint, and equal radii on two circles
    driven to 15 and 25 cannot hold."""
    gui.new_design()
    gui.enter_sketch("c")
    two_circles()
    cs = [e for e in gui.sketch()["entities"] if e["type"] == "circle"]
    gui.check("equal_radius: the circles start unequal", len(cs) == 2 and not near(cs[0]["radius"], cs[1]["radius"], 1e-6),
              f"radii {[round(c['radius'], 6) for c in cs]}")
    d1 = gui.constrain()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    pick("equal_radius", rim(cs[0]), rim(cs[1]))
    gui.press("Equal length")  # the shared Equal button, not Equal radius
    d = gui.sketch()
    ra, rb = d["entities"][ia]["radius"], d["entities"][ib]["radius"]
    gui.check("equal_radius: the radii are now equal", near(ra, rb, 1e-9), f"{ra:.9f} and {rb:.9f}")
    gui.check("equal_radius: and not equal at zero", ra > 1e-6, f"radius {ra:.9f}")
    d2 = confirm_and_reopen()
    survived("equal_radius", d2)
    rs = sorted(round(e["radius"], 9) for e in d2["entities"] if e["type"] == "circle")
    gui.check("equal_radius: still equal after the round trip", len(rs) == 2 and near(rs[0], rs[-1], 1e-9), f"radii {rs}")
    leave()


def rung_collinear():
    """Two offset lines brought onto one infinite line. Oblique on purpose: axis-aligned lines pick
    up an inferred Horizontal as they are drawn."""
    gui.new_design()
    gui.enter_sketch("l")
    gui.click_mm(-60, -14)
    gui.click_mm(-12, -6)
    keep_as_drawn()
    key("l")
    gui.click_mm(12, 14)
    gui.click_mm(60, 22)
    keep_as_drawn()
    ls = [e for e in gui.sketch()["entities"] if e["type"] == "line"]
    dy0 = abs(ls[0]["p0"][1] - ls[1]["p0"][1]) if len(ls) == 2 else None
    gui.check("collinear: the lines start on different lines", dy0 is not None and dy0 > 1.0,
              f"{len(ls)} lines, dy {dy0}")
    d1 = gui.constrain()
    ls = [e for e in d1["entities"] if e["type"] == "line"]
    ia, ib = d1["entities"].index(ls[0]), d1["entities"].index(ls[1])
    pick("collinear", mid(ls[0]), mid(ls[1]))
    gui.press("Collinear")
    d = gui.sketch()
    A, B = d["entities"][ia], d["entities"][ib]
    ax, ay = A["p0"]
    dx, dy = A["p1"][0] - ax, A["p1"][1] - ay
    crosses = [dx * (q[1] - ay) - dy * (q[0] - ax) for q in (B["p0"], B["p1"])]
    gui.check("collinear: both ends of the second line lie on the first", all(abs(c) < 1e-6 for c in crosses),
              f"cross products {[round(c, 9) for c in crosses]}")
    # A line collapsed to a point is collinear with anything, so both have to survive.
    gui.check("collinear: neither line collapsed", A["length"] > 1.0 and B["length"] > 1.0,
              f"lengths {A['length']:.6f}, {B['length']:.6f}")
    survived("collinear", confirm_and_reopen())
    leave()


def rung_distance_xy():
    """Horizontal distance: accepting the value its field opens with moves nothing, and a typed one
    drives the gap."""
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-30, -20)
    key("p")
    gui.click_mm(25, 18)
    # A third point, placed while the Point tool is armed: the typed half below needs a pair with
    # no dimension yet, and once Constrain mode has the canvas a click picks instead of placing.
    key("p")
    gui.click_mm(5, 5)
    ps = [e for e in gui.sketch()["entities"] if e["type"] == "point"]
    if not gui.check("distance_xy: three points placed", len(ps) == 3, f"{len(ps)} points"):
        return
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib, ic = (d1["entities"].index(p) for p in ps)
    # The no-op: the field opens with the current gap, and Return must change nothing. The
    # constraint is signed, (pB - pA) along the axis, so refs ordered to show |delta| while the
    # real delta is negative would send the point across its anchor on Return.
    before = [list(d1["entities"][ia]["p"]), list(d1["entities"][ib]["p"])]
    pick("distance_xy", ps[0]["p"], ps[1]["p"])
    gui.press("Horizontal distance")
    accept_shown_value()
    d = gui.sketch()
    after = [list(d["entities"][ia]["p"]), list(d["entities"][ib]["p"])]
    moved = max(abs(a - b) for pa, pb in zip(before, after) for a, b in zip(pa, pb))
    # 5 microns, not zero: the field shows the value rounded, so accepting it commits the rounded
    # value and the geometry may shift by up to half a shown unit. The defect this guards is a sign
    # flip, which moves the point by twice the gap.
    gui.check("distance_xy: accepting the shown value moved nothing", moved < 5e-3, f"moved {moved:.9f}")
    # Nothing moving would also pass if the constraint never applied, so the gap must be real.
    gx0 = abs(d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0])
    gui.check("distance_xy: the points still have a real horizontal gap", gx0 > 1.0, f"gap {gx0:.6f}")
    # Driven with a typed value on a fresh pair: the pair just dimensioned already has its
    # horizontal distance, and the button rightly opens no field to dimension it twice.
    dy_before = d["entities"][ic]["p"][1] - d["entities"][ia]["p"][1]
    pick("distance_xy", d["entities"][ia]["p"], d["entities"][ic]["p"])
    gui.press("Horizontal distance")
    gui.values(40)
    d = gui.sketch()
    gx = abs(d["entities"][ic]["p"][0] - d["entities"][ia]["p"][0])
    gy = d["entities"][ic]["p"][1] - d["entities"][ia]["p"][1]
    gui.check("distance_xy: the horizontal gap is the typed 40", near(gx, 40.0, 1e-6), f"gap {gx:.9f}")
    gui.check("distance_xy: the vertical gap is untouched", near(gy, dy_before, 1e-6), f"{dy_before:.9f} -> {gy:.9f}")
    survived("distance_xy", confirm_and_reopen())
    leave()


def rung_coincident_points():
    """Coincident on two points joins them: the simplest use of the button."""
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-30, -20)
    key("p")
    gui.click_mm(25, 18)
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    gap0 = math.dist(ps[0]["p"], ps[1]["p"])
    gui.check("coincident_points: the points start apart", gap0 > 1.0, f"gap {gap0:.6f}")
    pick("coincident_points", ps[0]["p"], ps[1]["p"])
    gui.press("Coincident")
    d = gui.sketch()
    gap = math.dist(d["entities"][ia]["p"], d["entities"][ib]["p"])
    gui.check("coincident_points: the points are now joined", gap < 1e-6, f"gap {gap:.9f}")
    d2 = confirm_and_reopen()
    survived("coincident_points", d2)
    ps2 = [e for e in d2["entities"] if e["type"] == "point"]
    gap2 = math.dist(ps2[0]["p"], ps2[1]["p"]) if len(ps2) == 2 else None
    gui.check("coincident_points: still joined after the round trip", gap2 is not None and gap2 < 1e-6,
              f"{len(ps2)} points, gap {gap2}")
    leave()


def rung_symmetric_axis():
    """Symmetric about the sketch's vertical axis, with two picks and no axis line drawn."""
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-40, 15)
    key("p")
    gui.click_mm(12, 15)
    d1 = gui.constrain()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    gui.check("symmetric_axis: the points start unmirrored", not near(abs(ps[0]["p"][0]), abs(ps[1]["p"][0]), 1e-3),
              f"x {ps[0]['p'][0]:.6f}, {ps[1]['p'][0]:.6f}")
    pick("symmetric_axis", ps[0]["p"], ps[1]["p"])
    gui.press("Symmetric about the vertical axis")
    d = gui.sketch()
    xa, xb = d["entities"][ia]["p"][0], d["entities"][ib]["p"][0]
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    gui.check("symmetric_axis: mirrored across x = 0", near(xa, -xb, 1e-9), f"x {xa:.9f} and {xb:.9f}")
    # Both collapsing onto the axis would satisfy the mirror trivially.
    gui.check("symmetric_axis: and not both collapsed onto the axis", abs(xa) > 1e-6, f"|x| {abs(xa):.9f}")
    gui.check("symmetric_axis: y untouched on both", near(ya, yb, 1e-6), f"y {ya:.6f}, {yb:.6f}")
    survived("symmetric_axis", confirm_and_reopen())
    leave()


def rung_type_guards():
    """A button that cannot apply to what is picked stores no constraint."""
    # Horizontal on a lone point: the point has no direction to make horizontal. A constraint
    # stored anyway would be listed in the panel and never act.
    gui.new_design()
    gui.enter_sketch("p")
    gui.click_mm(-30, -20)
    d = gui.constrain()
    ps = [e for e in d["entities"] if e["type"] == "point"]
    ia = d["entities"].index(ps[0])
    pick("type_guards", ps[0]["p"])
    gui.press("Horizontal")
    p = gui.sketch()["entities"][ia]["p"]
    gui.check("type_guards: Horizontal on a point moved nothing",
              near(p[0], ps[0]["p"][0], 1e-9) and near(p[1], ps[0]["p"][1], 1e-9), f"{ps[0]['p']} -> {p}")
    d2 = confirm_and_reopen()
    gui.check("type_guards: Horizontal on a point stored no constraint", d2["constraints"] == 0,
              f"{d2['constraints']} constraints")
    leave()

    # Angle on two circles: a circle has no direction, so there is no angle to measure or keep.
    gui.new_design()
    gui.enter_sketch("c")
    two_circles()
    d = gui.constrain()
    cs = [e for e in d["entities"] if e["type"] == "circle"]
    pick("type_guards", rim(cs[0]), rim(cs[1]))
    gui.press("Angle")
    # A field can open a moment after the press, so the canvas is watched for one for a second.
    field = until(lambda: gui.value_field() or gui.sketch()["editing"], timeout=1.0)
    gui.check("type_guards: Angle on two circles opened no value field", not field, f"field {field}")
    key("Escape")
    d2 = confirm_and_reopen()
    gui.check("type_guards: Angle on two circles stored no constraint", d2["constraints"] == 0,
              f"{d2['constraints']} constraints")
    leave()


# ---------------------------------------------------------------- history, files and size

def features():
    return gui.cad("describe_scene")["features"]


def rung_undo():
    """Undo in the open sketch takes the last entity away and leaves the others where they were."""
    gui.enter_sketch("l")
    draw_line(-50, -30, 0, -30, 50, 0)
    key("l")
    draw_line(0, -30, 0, 20, 50, 90)
    key("l")
    draw_line(0, 20, -40, 20, 40, 180)
    before = gui.sketch()["entities"]
    gui.check("undo: three entities drawn", len(before) == 3, f"{len(before)} entities")
    key("Ctrl+Z")
    after = gui.sketch()["entities"]
    gui.check("undo: one undo leaves two", len(after) == 2, f"{len(after)} entities")
    # Bit-identical, not re-solved: an undo that solved the sketch again could shift a survivor by
    # a rounding step.
    shifts = [(math.dist(a["p0"], b["p0"]), math.dist(a["p1"], b["p1"])) for a, b in zip(before, after)]
    gui.check("undo: the survivors are bit-identical", all(s == (0.0, 0.0) for s in shifts),
              f"endpoint shifts {shifts}")
    key("Ctrl+Z")
    n = len(gui.sketch()["entities"])
    gui.check("undo: a second undo drops one more", n == 1, f"{n} entities")
    gui.cancel()


def rung_feature_undo():
    """Undo and redo across the commit: a sketch deleted from the Feature tree comes back exactly.

    The keys go where the click on the tree's row left the keyboard, as a person's do."""
    gui.new_design()
    gui.enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    gui.confirm()
    feats = features()
    gui.check("feature_undo: one feature committed", len(feats) == 1, f"{len(feats)} features")
    if not feats:
        raise AssertionError("Confirm committed no feature to delete")
    gui.row(feats[-1]["name"])
    gui.keys("Delete")
    until(lambda: not features())
    left = features()
    gui.check("feature_undo: the tree is empty after Delete", not left, f"features {left}")
    gui.keys("Ctrl+Z")
    back = until(features)
    gui.check("feature_undo: undo brings the feature back", len(back) == 1, f"features {back}")
    d = gui.reopen()
    ls = lengths(d["entities"])
    gui.check("feature_undo: it is the same rectangle", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = d["closed_loops"]
    gui.check("feature_undo: area 9600", len(lp) == 1 and near(abs(lp[0]["area"]), 9600.0, 1e-6),
              f"areas {[abs(l['area']) for l in lp]}")
    gui.cancel()
    gui.keys("Ctrl+Y")
    until(lambda: not features())
    again = features()
    gui.check("feature_undo: redo removes it again", not again, f"features {again}")
    gui.new_design()


def unanswered(reply, doing):
    """Raise when a menu click is held by a dialog no rule answered."""
    if reply.get("status") == "blocked_by_dialog":
        raise RuntimeError(f"{doing} the project opened a dialog no rule answers: {reply}")


def rung_roundtrip():
    """Save the project and open it again: the profile comes back to the last decimal.

    Both go through the File menu. A rule answers each file picker with the project's path, as a
    person would type it; a GTK picker can't take a path that way, so the rung skips on Linux."""
    if sys.platform.startswith("linux"):
        gui.skip("roundtrip", "a GTK file picker can't take a path from MCP")
        return
    gui.new_design()
    gui.enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    key("r")
    gui.click_mm(-45, -15)
    gui.click_mm(-5, 15)
    gui.values(40, 30)
    key("c")
    gui.click_mm(30, 0)
    gui.click_mm(42, 0)
    gui.values(10)
    before = gui.sketch()
    gui.confirm()
    folder = tempfile.mkdtemp(prefix="orca-cad-roundtrip-")
    path = os.path.join(folder, "roundtrip.3mf")
    try:
        # The pickers are matched by their prompts, unanchored: a picker answered unshown has no
        # title. Save Project as, not Save: once the project has a path, Save writes to it with no
        # picker.
        gui.client.call("auto_answer_dialogs",
                        rules=[{"match": "Save file as", "button": "ok", "path": path, "overwrite": True}])
        unanswered(gui.menu("File>Save Project as..."), "saving")
        size = os.path.getsize(path) if os.path.isfile(path) else 0
        gui.check("roundtrip: the project was written", size > 0, f"{size} bytes at {path}")
        gui.new_design()  # empty the tree, then read it back from the file
        # Opening asks first whether to save the current project when it has unsaved changes. No:
        # saving would write the emptied design over the file about to be opened.
        gui.client.call("auto_answer_dialogs",
                        rules=[{"match": "Choose one file", "button": "ok", "path": path},
                               {"match": "unsaved changes", "button": "no"}])
        unanswered(gui.menu("File>Open Project..."), "opening")
    finally:
        gui.client.call("auto_answer_dialogs", rules=[])
        shutil.rmtree(folder, ignore_errors=True)
    # Opening a project lands on Prepare, and leaves the Design view fitted to the plates.
    gui.show_design()
    feats = until(features)
    gui.check("roundtrip: the opened project has one feature", len(feats) == 1, f"features {feats}")
    after = gui.reopen()

    def key_of(e):
        return (e["type"], tuple(round(c, 12) for c in (e.get("p0") or e.get("center") or e.get("p"))),
                round(e.get("length", e.get("radius", 0.0)), 12))

    b, a = sorted(map(key_of, before["entities"])), sorted(map(key_of, after["entities"]))
    gui.check("roundtrip: every entity identical to 12 decimals", a == b,
              f"{len(a)} entities, differing: {[x for x in a if x not in b]} vs {[x for x in b if x not in a]}")
    lp = after["closed_loops"]
    gui.check("roundtrip: three closed loops, buildable", len(lp) == 3 and after["buildable"],
              f"{len(lp)} closed loops, buildable {after['buildable']}")
    outer = max(range(len(lp)), key=lambda i: abs(lp[i]["area"])) if lp else None
    gui.check("roundtrip: the voids still belong to the outer loop",
              outer is not None and sorted(lp[outer]["holes"]) == [i for i in range(len(lp)) if i != outer],
              f"outer loop {outer} owns holes {lp[outer]['holes'] if lp else None}")
    gui.check("roundtrip: outer area 9600", outer is not None and near(abs(lp[outer]["area"]), 9600.0, 1e-9),
              f"outer area {abs(lp[outer]['area']):.9f}" if lp else "no closed loop")
    leave()


def rung_scale():
    """A gesture on top of a sketch that already holds over a thousand entities."""
    gui.enter_sketch("r")
    # A plate with a 20 x 15 grid of square cut-outs, 1204 lines, sized to the region the
    # calibration probes covered so every part of it can be clicked.
    x0, x1, y0, y1 = gui.safe
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    hw, hh = (x1 - x0) * 0.44, (y1 - y0) * 0.44
    ents = [{"type": "line", "p0": [cx - hw, cy - hh], "p1": [cx + hw, cy - hh]},
            {"type": "line", "p0": [cx + hw, cy - hh], "p1": [cx + hw, cy + hh]},
            {"type": "line", "p0": [cx + hw, cy + hh], "p1": [cx - hw, cy + hh]},
            {"type": "line", "p0": [cx - hw, cy + hh], "p1": [cx - hw, cy - hh]}]
    # The 300 cut-outs fill the left half. The right half stays clear, so the gesture below lands
    # out of snapping reach of every cut-out's corners.
    pitch_x, pitch_y = hw * 0.9 / 20.0, hh * 1.9 / 15.0
    side = min(pitch_x, pitch_y) * 0.4
    for i in range(20):
        for j in range(15):
            x = cx - hw * 0.95 + i * pitch_x
            y = cy - hh * 0.95 + j * pitch_y
            c = [(x, y), (x + side, y), (x + side, y + side), (x, y + side), (x, y)]
            for k in range(4):
                ents.append({"type": "line", "p0": list(c[k]), "p1": list(c[k + 1])})
    # The one call in this test that changes the sketch outside the UI, and part of what the rung
    # tests. It is the regression test for j7gc: a bulk sketch_add made while a creation tool is
    # armed, here the rectangle, was read as a drawn gesture, opened that tool's value field and
    # swallowed the next key and click until an Escape dismissed it. So no Escape follows: the
    # gesture below has to land on the first try. Where the plate comes from is not under test,
    # only that drawing still works with it on screen.
    t0 = time.monotonic()
    gui.cad("sketch_add", entities=ents)
    t_add = time.monotonic() - t0
    d0 = gui.sketch()
    gui.check("scale: every entity loaded", len(d0["entities"]) == len(ents),
              f"{len(d0['entities'])} of {len(ents)} entities, in {t_add * 1000:.0f} ms")
    lp0 = d0["closed_loops"]
    gui.check("scale: 301 closed loops", len(lp0) == 301, f"{len(lp0)} closed loops")
    outer = max(range(len(lp0)), key=lambda i: abs(lp0[i]["area"])) if lp0 else None
    gui.check("scale: the plate's area", outer is not None and near(abs(lp0[outer]["area"]), 4.0 * hw * hh, 1e-9),
              f"outer {abs(lp0[outer]['area']) if lp0 else None} vs {4.0 * hw * hh:.9f}")
    holes = lp0[outer]["holes"] if lp0 else []
    gui.check("scale: all 300 cut-outs belong to the plate", len(holes) == 300, f"{len(holes)} holes")
    gui.check("scale: every cut-out is exactly side squared",
              bool(holes) and all(near(abs(lp0[h]["area"]), side * side, 1e-9) for h in holes),
              f"side {side:.9f}, areas {sorted({round(abs(lp0[h]['area']), 12) for h in holes})[:5]}")
    # One more entity drawn by hand on top of all that.
    key("l")
    t0 = time.monotonic()
    ax, ay = cx + hw * 0.15, cy + hh * 0.55  # clear of the grid, inside the plate
    want_len = int(hw * 0.5)  # a whole number, typed as it is
    gui.click_mm(ax, ay)
    gui.click_mm(ax + want_len, ay)
    gui.value(want_len)
    dl = gui.sketch()
    gui.value(0)
    t_draw = time.monotonic() - t0
    d = gui.sketch()
    gui.check("scale: the gesture added exactly one entity", len(d["entities"]) == len(ents) + 1,
              f"{len(d['entities']) - len(ents)} added, in {t_draw:.1f} s")
    new = d["entities"][-1]
    gui.check("scale: it took its typed length exactly", near(new.get("length", math.nan), float(want_len), 1e-9),
              f"{new['type']} of length {new.get('length')} vs {want_len}; after the typed length: solve_ok "
              f"{dl['solve_ok']}, {dl['constraints']} constraints, dof {dl['dof']}, {len(dl['entities'])} entities")
    ang = math.nan
    if new["type"] == "line":
        ang = math.degrees(math.atan2(new["p1"][1] - new["p0"][1], new["p1"][0] - new["p0"][0])) % 360.0
    gui.check("scale: and its typed angle", near(ang, 0.0, 1e-9) or near(ang, 360.0, 1e-9), f"angle {ang} deg")
    moved = [i for i, (a, b) in enumerate(zip(d0["entities"], d["entities"]))
             if math.dist(a["p0"], b["p0"]) != 0.0 or math.dist(a["p1"], b["p1"]) != 0.0]
    gui.check("scale: none of the entities already there moved", not moved,
              f"{len(moved)} moved, the first {moved[:10]}")
    gui.cancel()


# ---------------------------------------------------------------- the runner's own recovery

def selfcheck_recovery():
    """Fail with as much left behind as a rung can leave: a tool armed, the mouse button held down on
    the canvas and the offer menu asked for (Shift+F10). The rungs run after it must pass."""
    gui.enter_sketch("l")
    u, v = gui.px(0, 0)
    gui.gesture(action="down", x=round(u), y=round(v))
    offer = gui.keys("Shift+F10").get("popup_menu", {}).get("items", [])
    raise RuntimeError(f"left a tool armed, the mouse button down and the offer asked for ({offer[:1]})")


RUNGS = {"rect": rung_rect, "circle": rung_circle, "line": rung_line, "arc": rung_arc,
         "slot": rung_slot, "polygon": rung_polygon, "ellipse": rung_ellipse,
         "point": rung_point, "spline": rung_spline, "voids": rung_voids,
         "fillet": rung_fillet, "chamfer": rung_chamfer, "offset": rung_offset,
         "mirror": rung_mirror, "mirror_arcs": rung_mirror_arcs, "trim": rung_trim, "extend": rung_extend,
         "dimension": rung_dimension, "constrain": rung_constrain,
         "perpendicular": rung_perpendicular,
         "equal_radius": rung_equal_radius, "collinear": rung_collinear,
         "distance_xy": rung_distance_xy, "symmetric_axis": rung_symmetric_axis,
         "coincident_points": rung_coincident_points, "type_guards": rung_type_guards,
         "parallel": rung_parallel, "live_constrain": rung_live_constrain, "badge": rung_badge,
         "vertical": rung_vertical, "equal_radius_button": rung_equal_radius_button,
         "concentric": rung_concentric, "tangent": rung_tangent, "midpoint": rung_midpoint,
         "symmetric_line_axis": rung_symmetric_line_axis, "symmetric_h": rung_symmetric_h,
         "radius": rung_radius, "diameter": rung_diameter, "fix": rung_fix,
         "distance_y": rung_distance_y, "undo": rung_undo, "feature_undo": rung_feature_undo,
         "roundtrip": rung_roundtrip, "scale": rung_scale}


def main():
    global gui
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()
    return gui.run({**RUNGS, "selfcheck-recovery": selfcheck_recovery}, sys.argv[1:] or list(RUNGS))


if __name__ == "__main__":
    sys.exit(main())
