#!/usr/bin/env python3
"""Checks that sketches drawn on the Design tab the way a person draws them come out as typed.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_cad_sketching.py [rung ...]

With no rung named, every rung runs. Each one opens a sketch on the XY plane (Shift+S, then a click
on the plane), draws with clicks on the canvas, types the exact values into the canvas's value field
with no click into it, and grades the geometry and loops sketch_describe reads back. It covers what
the Design commands that build sketches skip: the gesture state, the queue of value fields, snapping,
inference at gesture tolerance and the keyboard reaching the field. The click only has to be roughly
right; the typed number is what must come back exactly, and the shape is drawn away from it, so a
field that commits the size as drawn fails. Every action goes through the MCP UI tools, and the
Design commands only read, save the scale rung's one bulk add. Exits 0 when every check holds.

The rungs: each 2D tool draws its primitive and takes its exact values from the keyboard (rect,
circle, line, arc, slot, polygon, ellipse, point, spline), and voids draws one closed outer profile
with two internal voids, all by gesture in one sketch, and checks that the outer loop owns both.
The edit operations (fillet, chamfer, offset, mirror, mirror_arcs, trim, extend) are driven by the
picks and the on-geometry value label a person uses, and dimension puts a typed length on a drawn
shape. relations presses each of the twenty constraint buttons on shapes drawn for it, all in one
sketch: Parallel once in the open sketch, the rest in Constrain mode (K), Horizontal on a point and
Angle on two circles among them, which must store nothing. It grades the geometry each button
produced, then commits the sketch, opens it again and reads every relation back, its geometry and
the constraint itself. badge clicks a constraint's badge, which selects it and deletes nothing, as
does a double-click, then deletes it with Delete. undo and feature_undo undo inside a sketch and
across its commit, from the keyboard. roundtrip saves the project and opens it again through the
File menu, its file pickers answered by dialog_auto_answer rules with the path. scale draws on top
of a sketch holding 1204 entities, which it loads with one sketch_draw command while a tool is
armed; the call says why. selfcheck-recovery runs only when named: it raises with the mouse button
held, the offer menu asked for and a tool armed, to show that the rungs after it start clean.
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
# Each shape is drawn away from every value typed into it. A field the digits never reach still
# commits on Return, with the value it opened with, the size as drawn, so the typed value read back
# is what tells that from a field that took the keys.

def rung_rect():
    gui.enter_sketch("r")
    gui.click_mm(-60, -40)
    gui.click_mm(45, 25)  # 105 x 65
    gui.values(120, 80)
    d = gui.sketch()
    ls = lengths(d["entities"])
    gui.check("rect: the sides are the typed 120 x 80", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    xs = [p[0] for e in d["entities"] for p in (e["p0"], e["p1"])]
    ys = [p[1] for e in d["entities"] for p in (e["p0"], e["p1"])]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    gui.check("rect: the first field's 120 is its width along x and the second's 80 its height along y",
              near(w, 120.0, 1e-9) and near(h, 80.0, 1e-9), f"{w:.9f} along x, {h:.9f} along y")
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
    gui.click_mm(15, -5)  # 57 long at 15 deg, clear of the direction snap's 0 and 30
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
    gui.click_mm(0, 20)  # through the three clicks: radius 50, sweep 106 deg
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
    gui.click_mm(20, 8)   # centres 50.6 apart, at 9 deg
    gui.click_mm(18, 20)  # 12 off the centre line: 24 wide
    # The fields are the centre distance, the slot's full width and its angle: a width of 20 gives
    # the end radius of 10 graded below.
    gui.values(60, 20, 0)
    d = gui.sketch()
    arcs = [x for x in d["entities"] if x["type"] == "arc"]
    lns = [x for x in d["entities"] if x["type"] == "line"]
    gui.check("slot: its flanks lie at the typed angle 0",
              len(lns) == 2 and all(near(l["p1"][1] - l["p0"][1], 0.0, 1e-9) for l in lns),
              f"flanks rise {[round(l['p1'][1] - l['p0'][1], 9) for l in lns]}")
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
    gui.check("ellipse: semi-major the typed 50 and semi-minor the typed 20",
              len(e) == 1 and near(e[0]["radius"], 50.0) and near(e[0]["rminor"], 20.0),
              f"semi-axes {[(x['radius'], x['rminor']) for x in e]}")
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
    label heights, a label height being 15 GL pixels in millimetres at `at`."""
    th = max(gui.budget(15.0) * gui.mm_per_px(*at), 1e-4)
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
    if until(gui.value_field) is None:
        raise AssertionError("the button opened no value field")
    field = until(gui.typing_field)
    if field is None:
        raise AssertionError(f"the {gui.value_field().title} field never took the keyboard")
    gui.keys("Return")
    if not until(lambda: gui.value_field() is None):
        raise AssertionError(f"Return left the {field.title} field open")


def pick(rung, *points, of=""):
    """In Constrain mode, drop the picks a constraint button left held, then click each point, and
    check that every click picked an entity of its own; returns whether they all did. `of` ends the
    check's name, for a rung that picks the same number twice.

    A click the plane map put off its entity picks nothing, and the button pressed next acts on
    fewer entities than the rung means, or on none, which a check that only reads geometry would
    not notice. The panel's status line counts the picks. A button leaves its picks held, and a
    click on an entity not among them would add to them, so the picks are dropped first with a
    click on empty space. The count then reaches one per click only when every click landed on a
    different entity."""
    gui.click_mm(*EMPTY)
    until(lambda: gui.picked() == 0)
    counts = []
    for n, p in enumerate(points, 1):
        gui.click_mm(*p)
        until(lambda: gui.picked() == n)
        counts.append(gui.picked())
    what = {1: "the pick", 2: "both picks", 3: "all three picks"}[len(points)]
    return gui.check(f"{rung}: {what} landed on an entity{of}", counts == list(range(1, len(points) + 1)),
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
    """Leave the sketch, discarding what it holds, and empty the design: a rung that commits its
    sketch leaves a feature, and the rungs after it start from none."""
    gui.cancel()
    gui.new_design()


# What the sketch solver holds a solved sketch to. libslvs stops when every equation's residual is within
# System::CONVERGE_TOLERANCE, LENGTH_EPS / 1e2 = 1e-8 (libslvs system.cpp:20, LENGTH_EPS in solvespace.h:127), and
# the Design tab keeps a solve's geometry only when it converged (SketchSolver.cpp:449, ok is SLVS_RESULT_OKAY, and
# :486, nothing is written back otherwise). A Parallel's equation in the sketch's plane is the pseudo-scalar product of the
# two lines' directions, in mm² (constrainteq.cpp, Type::PARALLEL: a.x * b.y - a.y * b.x), so the angle it leaves
# shrinks with the lines' lengths; a Perpendicular's is their direction cosine, which bounds the angle itself.
SOLVE_TOLERANCE = 1e-8


def direction_cross(a, b):
    """The pseudo-scalar product of two lines' directions, p0 to p1, in mm²: what a Parallel solves to zero."""
    return ((a["p1"][0] - a["p0"][0]) * (b["p1"][1] - b["p0"][1])
            - (a["p1"][1] - a["p0"][1]) * (b["p1"][0] - b["p0"][0]))


def angle_between(a, b):
    """The angle between two lines' directions, p0 to p1, in degrees from 0 to 180: atan2 of their cross and dot
    products, which reads it to a double's precision at every angle. acos of the cosine would read nothing finer than
    about 1e-6 deg near 0 and 180, where one rounding step of the cosine is 1.5e-8 rad."""
    dot = ((a["p1"][0] - a["p0"][0]) * (b["p1"][0] - b["p0"][0])
           + (a["p1"][1] - a["p0"][1]) * (b["p1"][1] - b["p0"][1]))
    return math.degrees(math.atan2(abs(direction_cross(a, b)), dot))


def parallel_gap(a, b):
    """How far from parallel, in degrees. Parallel reads 0 or 180; both mean parallel."""
    ang = angle_between(a, b)
    return min(ang, 180.0 - ang)


def rim(e, deg=135.0):
    """A point on a circle's rim, away from +X: a click on the +X point grabs the radius grip
    instead of picking the circle."""
    a = math.radians(deg)
    return (e["center"][0] + e["radius"] * math.cos(a), e["center"][1] + e["radius"] * math.sin(a))


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


def rung_badge():
    """A click on a constraint badge selects the constraint and deletes nothing, and Delete then
    removes that one constraint. A badge is small and sits where a person aims at the geometry, so a
    click that deleted would lose a constraint to a slip of the pointer. The badge clicked is the
    Horizontal of the rectangle's first side, found by its place in that side's stack."""
    gui.enter_sketch("r")
    draw_rect_undimensioned()
    side = gui.sketch()["entities"][0]
    cons = gui.cad("sketch_list_constraints")["items"]
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
    left = gui.cad("sketch_list_constraints")["items"]
    gui.check("badge: Delete removed one constraint", len(left) == len(cons) - 1,
              f"{len(cons)} constraints before Delete, {len(left)} after")
    gui.check("badge: the one removed is the side's Horizontal",
              not any(c["type"] == "horizontal" and c["refs"][0].get("entity") == side["index"] for c in left),
              f"constraints left: {[c['type'] for c in left]}")
    gui.check("badge: nothing stays selected after Delete", gui.sketch()["selected_constraint"] is None)
    leave()


# ---------------------------------------------------------------- every relation in one sketch
# The relations rung presses each constraint button on shapes drawn for it, all in one sketch,
# which it commits and opens again once. The reopened sketch is read twice for each relation: its
# geometry, and the constraint itself in sketch_list_constraints, since geometry that already
# satisfies a relation goes on satisfying it when the constraint is lost on the way.
#
# Each relation's shapes are drawn in a cell of their own, in a grid of 70 x 45 mm cells about the
# sketch's origin, far enough from the other cells' shapes that no click snaps (8 px, 5 mm) to one of
# them, which would join two cells with an inferred Coincident. The circles of all the cells are drawn
# 1.5 mm or more apart in radius: a circle drawn within 1% of another's radius, anywhere in the
# sketch, is made Equal radius to it, which would tie two cells together too, and two clicks rounded
# to whole pixels put a radius up to 1.24 mm off the one asked for. The middle cell stays empty, out
# of a Constrain pick's reach (24 px, 15 mm) of every shape: a click on the origin is on empty space.
# Symmetric about the vertical axis has the middle column and Symmetric about the horizontal axis the
# middle row, so their points start either side of the axis they mirror across.
COL, ROW = 70.0, 45.0
EMPTY = (0.0, 0.0)

# The degrees of freedom the reopened sketch is left with, cell by cell: what its shapes have (a
# point 2, a line 4, a circle 3), less what the constraints inferred while drawing take (a
# Coincident 2, a Horizontal or Vertical 1) and what the buttons take (Concentric, Midpoint,
# Symmetric, Fix, Collinear and Coincident 2, the rest 1).
DOF = {"equal_length": 16 - 8 - 4 - 1,              # rectangle: 4 Coincident, 2 Horizontal, 2 Vertical
       "perpendicular": 8 - 2 - 1, "parallel": 8 - 2 - 1, "live_parallel": 8 - 2 - 1,   # joined lines
       "vertical": 4 - 1, "concentric": 6 - 2,
       "tangent": 7 - 1 - 1,                         # the line is drawn Horizontal
       "midpoint": 6 - 2, "symmetric_line": 8 - 1 - 2,   # the axis is drawn Vertical
       "symmetric_h": 4 - 2, "symmetric_v": 4 - 2, "radius": 3 - 1, "diameter": 3 - 1,
       "fix": 4 - 2 - 1, "distance_y": 4 - 1, "distance_x": 6 - 1 - 1, "equal_radius": 6 - 1,
       "equal_radius_button": 6 - 1, "collinear": 8 - 2, "coincident": 4 - 2}


def anchor(e):
    """Where an entity is, for telling which cell it is in: a line's middle, a round entity's
    centre, a point itself."""
    if e["type"] == "line":
        return mid(e)
    return e["p"] if e["type"] == "point" else e["center"]


def in_cell(d, o, kinds):
    """The entities of sketch_describe `d` in the cell centred on `o`, in index order; raises unless
    they are of `kinds`, in that order."""
    got = [e for e in d["entities"]
           if abs(anchor(e)[0] - o[0]) < COL / 2.0 and abs(anchor(e)[1] - o[1]) < ROW / 2.0]
    if [e["type"] for e in got] != list(kinds):
        raise AssertionError(f"the cell at {o} holds {[e['type'] for e in got]}, not {list(kinds)}")
    return got


def at(o, x, y):
    return (o[0] + x, o[1] + y)


def line_at(o, a, b):
    """A line from a to b, in mm from the cell's centre o, kept as drawn; the tool back to Select."""
    key("l")
    gui.click_mm(*at(o, *a))
    gui.click_mm(*at(o, *b))
    keep_as_drawn()


def circle_at(o, c, r):
    """A circle about c, in mm from o, of radius r, its radius field dismissed."""
    key("c")
    gui.click_mm(*at(o, *c))
    gui.click_mm(*at(o, c[0] + r, c[1]))
    escape_field()


def point_at(o, p):
    key("p")
    gui.click_mm(*at(o, *p))


def two_lines(o):
    """Two free lines joined at one end, about 50 degrees apart: well outside inference's snap to
    parallel or perpendicular, so the button under test has the work to do."""
    line_at(o, (-25, -15), (15, -9))
    line_at(o, (15, -9), (27.5, 17.5))


def now(d, *ents):
    """The entities `ents` as sketch_describe `d` has them."""
    return [d["entities"][e["index"]] for e in ents]


def bound(cons, ents):
    """The types of the constraints in `cons` (sketch_list_constraints' items) naming any of `ents`."""
    want = {e["index"] for e in ents}
    return sorted(c["type"] for c in cons if want & {r.get("entity") for r in c["refs"]})


def stored(rung, cons, kind, ents, value=None, what=None):
    """Check that `cons` (sketch_list_constraints' items) holds exactly one `kind` constraint on the
    entities `ents`, and of `value` when one is given (its size: a distance is signed by the order
    of its points). `what` names the constraint in the check, when the rung stores two of a kind."""
    want = {e["index"] for e in ents}
    got = [c for c in cons if c["type"] == kind and {r.get("entity") for r in c["refs"]} == want]
    ok = len(got) == 1 and (value is None or near(abs(got[0].get("value", math.nan)), value, 1e-9))
    gui.check(f"{rung}: the reopened sketch holds {what or 'the ' + kind}", ok,
              f"{len(got)} {kind} on entities {sorted(want)}"
              + (f" of value {got[0].get('value')}" if got and value is not None else "")
              + f"; the constraints on them: {bound(cons, ents)}")


# Each relation is a generator. It draws its shapes in the cell centred on o and yields. It is sent
# the committed sketch in Constrain mode, reads its shapes from it, presses its button there, checks
# what the button did and yields. It is sent the reopened sketch and its constraints, and checks the
# relation again in them. Its shapes keep their indices from Constrain mode on, which
# rung_relations() checks.

def rel_equal_length(o):
    """Equal length on two adjacent sides of a rectangle."""
    key("r")
    gui.click_mm(*at(o, -24, -16))
    gui.click_mm(*at(o, 24, 16))
    keep_as_drawn()
    d1 = yield
    a, b = corner_pair(in_cell(d1, o, ["line"] * 4))
    gui.check("equal_length: the two sides start unequal", not near(a["length"], b["length"], 1e-6),
              f"{a['length']:.6f} vs {b['length']:.6f}")
    pick("equal_length", mid(a), mid(b))
    gui.press("Equal length")
    A, B = now(gui.sketch(), a, b)
    gui.check("equal_length: the two sides are now equal", near(A["length"], B["length"], 1e-9),
              f"{A['length']:.9f} and {B['length']:.9f}")
    d2, cons = yield
    # The rectangle's own constraints keep opposite sides equal, so all four are.
    sides = sorted(round(e["length"], 9) for e in in_cell(d2, o, ["line"] * 4))
    gui.check("equal_length: a square after the round trip", near(sides[0], sides[-1], 1e-9), f"sides {sides}")
    stored("equal_length", cons, "equal_length", [a, b])


def rel_perpendicular(o):
    two_lines(o)
    d1 = yield
    a, b = in_cell(d1, o, ["line", "line"])
    a0 = angle_between(a, b)
    gui.check("perpendicular: the lines start off square", abs(a0 - 90.0) > 1e-3, f"{a0:.6f} deg")
    pick("perpendicular", mid(a), mid(b))
    gui.press("Perpendicular")
    ang = angle_between(*now(gui.sketch(), a, b))
    # 1e-6 deg: the solve stops once the direction cosine is within SOLVE_TOLERANCE, an angle of 1e-8
    # rad (5.7e-7 deg), from a 50 deg start. The reopened sketch is solved again from geometry already
    # that close, and Newton's step from there lands far inside the tolerance: it is held to exactly 90.
    gui.check("perpendicular: the lines are now at 90 deg", near(ang, 90.0, 1e-6), f"{ang:.9f} deg")
    d2, cons = yield
    ang = angle_between(*now(d2, a, b))
    gui.check("perpendicular: still exactly 90 deg after the round trip", near(ang, 90.0, 1e-9), f"{ang:.9f} deg")
    stored("perpendicular", cons, "perpendicular", [a, b])


def parallel_holds(rung, d, a, b, after):
    """Check that the lines a and b are parallel in `d` as the solver holds a Parallel, the product of their
    directions within SOLVE_TOLERANCE mm², and that neither collapsed to get there, which would meet it too."""
    A, B = now(d, a, b)
    c = direction_cross(A, B)
    gui.check(f"{rung}: the lines are parallel{after}", abs(c) <= SOLVE_TOLERANCE,
              f"|a x b| {abs(c):.3e} mm2 for lines of {A['length']:.3f} and {B['length']:.3f} mm, "
              f"{parallel_gap(A, B):.3e} deg from parallel")
    gui.check(f"{rung}: neither line collapsed{after}", A["length"] > 1.0 and B["length"] > 1.0,
              f"lengths {A['length']:.6f}, {B['length']:.6f}")


def rel_parallel(o):
    two_lines(o)
    d1 = yield
    a, b = in_cell(d1, o, ["line", "line"])
    g0 = parallel_gap(a, b)
    gui.check("parallel: the lines start off parallel", g0 > 1e-3, f"{g0:.6f} deg from parallel")
    pick("parallel", mid(a), mid(b))
    gui.press("Parallel")
    parallel_holds("parallel", gui.sketch(), a, b, "")
    d2, cons = yield
    parallel_holds("parallel", d2, a, b, " after the round trip")
    stored("parallel", cons, "parallel", [a, b])


def rel_live_parallel(o):
    """Parallel applied in the open sketch, as it is drawn: no K, no commit, no tree pick."""
    two_lines(o)
    a, b = in_cell(gui.sketch(), o, ["line", "line"])
    g0 = parallel_gap(a, b)
    gui.check("live_parallel: the lines start off parallel", g0 > 1e-3, f"{g0:.6f} deg from parallel")
    # Both picked in the open sketch, the second with Ctrl so it adds to the first rather than
    # replacing it, then the button: no K, which is the step this shows is not needed.
    gui.click_mm(*mid(a))
    gui.click_mm(*mid(b), modifiers=["ctrl"])
    sel = sorted(gui.sketch()["selection"])
    gui.check("live_parallel: both picks landed, the second adding to the first", sel == [a["index"], b["index"]],
              f"selection {sel}, lines {[a['index'], b['index']]}")
    gui.press("Parallel")
    parallel_holds("live_parallel", gui.sketch(), a, b, " without leaving the sketch")
    d1 = yield
    a, b = in_cell(d1, o, ["line", "line"])
    d2, cons = yield
    parallel_holds("live_parallel", d2, a, b, " after the round trip")
    stored("live_parallel", cons, "parallel", [a, b])


def rel_vertical(o):
    line_at(o, (-10, -17), (2.5, 15))
    d1 = yield
    (a,) = in_cell(d1, o, ["line"])
    dx0 = abs(a["p1"][0] - a["p0"][0])
    gui.check("vertical: the line starts off plumb", dx0 > 1.0, f"dx {dx0:.6f}")
    pick("vertical", mid(a))
    gui.press("Vertical")

    def holds(d, after):
        (e,) = now(d, a)
        dx = abs(e["p1"][0] - e["p0"][0])
        gui.check(f"vertical: the line is plumb{after}", near(dx, 0.0, 1e-6), f"dx {dx:.9f}")
        gui.check(f"vertical: the line did not collapse{after}", e["length"] > 1.0, f"length {e['length']:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("vertical", cons, "vertical", [a])


def rel_concentric(o):
    circle_at(o, (-15, -5), 8.0)
    circle_at(o, (14, 7), 14.0)
    d1 = yield
    a, b = in_cell(d1, o, ["circle", "circle"])
    gap0 = math.dist(a["center"], b["center"])
    gui.check("concentric: the centres start apart", gap0 > 1.0, f"{gap0:.6f} mm apart")
    pick("concentric", rim(a), rim(b))
    gui.press("Concentric")

    def holds(d, after):
        A, B = now(d, a, b)
        gap = math.dist(A["center"], B["center"])
        gui.check(f"concentric: the centres coincide{after}", near(gap, 0.0, 1e-6), f"{gap:.9f} mm apart")
        # Concentric is about centres only: a solve that also equalised the radii would be wrong.
        r0, r1 = [a["radius"], b["radius"]], [A["radius"], B["radius"]]
        gui.check(f"concentric: the radii are untouched{after}", all(near(x, y, 1e-6) for x, y in zip(r0, r1)),
                  f"radii {r0} -> {r1}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("concentric", cons, "concentric", [a, b])


def rel_tangent(o):
    circle_at(o, (0, -6), 9.5)
    # A line that clearly misses the circle, so tangency has to close the gap. It is within the
    # direction snap of horizontal, so it is drawn horizontal and held so.
    line_at(o, (-22, 13), (22, 16))
    d1 = yield
    c, ln = in_cell(d1, o, ["circle", "line"])
    gap0 = point_line_dist(c["center"], ln) - c["radius"]
    gui.check("tangent: the line starts clear of the circle", gap0 > 1.0, f"it misses by {gap0:.6f} mm")
    pick("tangent", rim(c), mid(ln))
    gui.press("Tangent")

    def holds(d, after):
        C, L = now(d, c, ln)
        dist = point_line_dist(C["center"], L)
        gui.check(f"tangent: the line touches the circle{after}", near(dist, C["radius"], 1e-6),
                  f"centre to line {dist:.9f}, radius {C['radius']:.9f}")
        gui.check(f"tangent: the circle did not collapse{after}", C["radius"] > 1e-6, f"radius {C['radius']:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("tangent", cons, "tangent", [c, ln])


def rel_midpoint(o):
    line_at(o, (-22, -9), (18, 12))
    point_at(o, (4.5, -16))
    d1 = yield
    ln, p = in_cell(d1, o, ["line", "point"])
    off0 = math.dist(p["p"], mid(ln))
    gui.check("midpoint: the point starts off the line's middle", off0 > 1.0, f"{off0:.6f} mm off")
    # The point first, then the line: Midpoint binds a point on the first pick and a whole segment
    # on the second.
    pick("midpoint", p["p"], mid(ln))
    gui.press("Midpoint")

    def holds(d, after):
        L, P = now(d, ln, p)
        off = math.dist(P["p"], mid(L))
        gui.check(f"midpoint: the point sits on the line's middle{after}", near(off, 0.0, 1e-6), f"{off:.9f} mm off")
        gui.check(f"midpoint: the line did not collapse{after}", L["length"] > 1.0, f"length {L['length']:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("midpoint", cons, "midpoint", [p, ln])


def rel_symmetric_line(o):
    """Symmetric about a picked axis line: two points and the axis, the only button that takes a
    third pick."""
    line_at(o, (0, -20), (0, 20))  # the axis: a real line, drawn vertical and held so
    point_at(o, (-19, 6))
    point_at(o, (9, 6))
    d1 = yield
    ax, p, q = in_cell(d1, o, ["line", "point", "point"])
    x = ax["p0"][0]
    gui.check("symmetric_line: the points start unmirrored", not near(abs(p["p"][0] - x), abs(q["p"][0] - x), 1e-3),
              f"{p['p'][0] - x:.6f} and {q['p'][0] - x:.6f} from the axis")
    pick("symmetric_line", p["p"], q["p"], mid(ax))
    gui.press("Symmetric")

    def holds(d, after):
        # Measured against the axis where it is now: all three entities are free, so the solver
        # may satisfy the mirror by moving the axis instead of the points.
        A, P, Q = now(d, ax, p, q)
        sp, sq = signed_point_line_dist(P["p"], A), signed_point_line_dist(Q["p"], A)
        gui.check(f"symmetric_line: mirrored about the picked line{after}", near(sp, -sq, 1e-6),
                  f"{sp:.9f} and {sq:.9f} from it")
        gui.check(f"symmetric_line: and not both collapsed onto it{after}", abs(sp) > 1e-6, f"|d| {abs(sp):.9f}")
        gui.check(f"symmetric_line: level with each other{after}", near(P["p"][1], Q["p"][1], 1e-6),
                  f"y {P['p'][1]:.6f}, {Q['p'][1]:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("symmetric_line", cons, "symmetric", [p, q, ax])


def rel_symmetric_h(o):
    """Symmetric about the sketch's horizontal axis, with two picks and no axis line drawn."""
    point_at(o, (0, -17))
    point_at(o, (0, 6))
    d1 = yield
    p, q = in_cell(d1, o, ["point", "point"])
    gui.check("symmetric_h: the points start unmirrored", not near(abs(p["p"][1]), abs(q["p"][1]), 1e-3),
              f"y {p['p'][1]:.6f}, {q['p'][1]:.6f}")
    pick("symmetric_h", p["p"], q["p"])
    gui.press("Symmetric about the horizontal axis")

    def holds(d, after):
        (xa, ya), (xb, yb) = (e["p"] for e in now(d, p, q))
        gui.check(f"symmetric_h: mirrored across y = 0{after}", near(ya, -yb, 1e-9), f"y {ya:.9f} and {yb:.9f}")
        gui.check(f"symmetric_h: and not both collapsed onto the axis{after}", abs(ya) > 1e-6, f"|y| {abs(ya):.9f}")
        gui.check(f"symmetric_h: x untouched on both{after}", near(xa, xb, 1e-6), f"x {xa:.6f}, {xb:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("symmetric_h", cons, "symmetric_about_x", [p, q])


def rel_symmetric_v(o):
    """Symmetric about the sketch's vertical axis, with two picks and no axis line drawn."""
    point_at(o, (-18, 0))
    point_at(o, (8, 0))
    d1 = yield
    p, q = in_cell(d1, o, ["point", "point"])
    gui.check("symmetric_v: the points start unmirrored", not near(abs(p["p"][0]), abs(q["p"][0]), 1e-3),
              f"x {p['p'][0]:.6f}, {q['p'][0]:.6f}")
    pick("symmetric_v", p["p"], q["p"])
    gui.press("Symmetric about the vertical axis")

    def holds(d, after):
        (xa, ya), (xb, yb) = (e["p"] for e in now(d, p, q))
        gui.check(f"symmetric_v: mirrored across x = 0{after}", near(xa, -xb, 1e-9), f"x {xa:.9f} and {xb:.9f}")
        # Both collapsing onto the axis would satisfy the mirror trivially.
        gui.check(f"symmetric_v: and not both collapsed onto the axis{after}", abs(xa) > 1e-6, f"|x| {abs(xa):.9f}")
        gui.check(f"symmetric_v: y untouched on both{after}", near(ya, yb, 1e-6), f"y {ya:.6f}, {yb:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("symmetric_v", cons, "symmetric_about_y", [p, q])


def rel_radius(o):
    circle_at(o, (0, 0), 15.5)
    d1 = yield
    (c,) = in_cell(d1, o, ["circle"])
    gui.check("radius: the circle starts at another radius", not near(c["radius"], 10.0, 1e-3),
              f"radius {c['radius']:.6f}")
    pick("radius", rim(c))
    gui.press("Radius")
    gui.values(10)

    def holds(d, after):
        (C,) = now(d, c)
        gui.check(f"radius: the radius is the typed 10{after}", near(C["radius"], 10.0, 1e-6), f"radius {C['radius']:.9f}")
        # A Radius constraint moves the rim, never the centre.
        moved = math.dist(C["center"], c["center"])
        gui.check(f"radius: the centre stayed put{after}", near(moved, 0.0, 1e-6), f"moved {moved:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("radius", cons, "radius", [c], 10.0)


def rel_diameter(o):
    """A typed Diameter is a diameter, not a radius: Diameter wired to the Radius handler would give
    r = 24 for a typed 24, and nothing about the sketch would look wrong."""
    circle_at(o, (0, 0), 17.0)
    d1 = yield
    (c,) = in_cell(d1, o, ["circle"])
    pick("diameter", rim(c))
    gui.press("Diameter")
    gui.values(24)

    def holds(d, after):
        r = now(d, c)[0]["radius"]
        gui.check(f"diameter: a typed diameter of 24 gives radius 12{after}", near(r, 12.0, 1e-6), f"radius {r:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("diameter", cons, "diameter", [c], 24.0)


def rel_fix(o):
    """Fix anchors a point, and the anchor is what the solver moves around. Fix alone is
    unfalsifiable, since nothing moves; a second constraint that would otherwise move the fixed
    point shows which end travels."""
    point_at(o, (-15, 0))
    point_at(o, (5, 0))
    d1 = yield
    a, b = in_cell(d1, o, ["point", "point"])
    pick("fix", a["p"])
    gui.press("Fix point (anchor in place)")
    A, B = now(gui.sketch(), a, b)
    moved = math.dist(A["p"], a["p"])
    gui.check("fix: the fixed point did not move when it was fixed", near(moved, 0.0, 1e-9), f"moved {moved:.9f}")
    # A horizontal gap far from the current one: only the free point may travel.
    pick("fix", A["p"], B["p"])
    gui.press("Horizontal distance")
    gui.values(35)

    def holds(d, after):
        A, B = now(d, a, b)
        gap = abs(B["p"][0] - A["p"][0])
        gui.check(f"fix: the gap is the typed 35{after}", near(gap, 35.0, 1e-6), f"gap {gap:.9f}")
        moved = math.dist(A["p"], a["p"])
        gui.check(f"fix: the anchored point held station{after}", near(moved, 0.0, 1e-6), f"moved {moved:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("fix", cons, "fix", [a])
    stored("fix", cons, "distance_x", [a, b], 35.0)


def rel_distance_y(o):
    """Vertical distance, which is not the straight-line one either."""
    point_at(o, (-11, -9))
    point_at(o, (9.5, 12))
    d1 = yield
    a, b = in_cell(d1, o, ["point", "point"])
    dx0 = b["p"][0] - a["p"][0]
    pick("distance_y", a["p"], b["p"])
    gui.press("Vertical distance")
    gui.values(30)

    def holds(d, after):
        A, B = now(d, a, b)
        gy, gx = abs(B["p"][1] - A["p"][1]), B["p"][0] - A["p"][0]
        gui.check(f"distance_y: the vertical gap is the typed 30{after}", near(gy, 30.0, 1e-6), f"gap {gy:.9f}")
        gui.check(f"distance_y: the horizontal gap is untouched{after}", near(gx, dx0, 1e-6), f"{dx0:.9f} -> {gx:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("distance_y", cons, "distance_y", [a, b], 30.0)


def rel_distance_x(o):
    """Horizontal distance: accepting the value its field opens with moves nothing, and a typed one
    drives the gap."""
    point_at(o, (-13.5, -9))
    point_at(o, (11.3, 8.1))
    point_at(o, (2.3, 2.3))
    d1 = yield
    a, b, c = in_cell(d1, o, ["point", "point", "point"])
    # The no-op: the field opens with the current gap, and Return must change nothing. The
    # constraint is signed, (pB - pA) along the axis, so refs ordered to show |delta| while the
    # real delta is negative would send the point across its anchor on Return.
    pick("distance_x", a["p"], b["p"])
    gui.press("Horizontal distance")
    accept_shown_value()
    A, B = now(gui.sketch(), a, b)
    moved = max(abs(u - v) for e, E in ((a, A), (b, B)) for u, v in zip(e["p"], E["p"]))
    # 5 microns, not zero: the field shows the value rounded, so accepting it commits the rounded
    # value and the geometry may shift by up to half a shown unit. The defect this guards is a sign
    # flip, which moves the point by twice the gap.
    gui.check("distance_x: accepting the shown value moved nothing", moved < 5e-3, f"moved {moved:.9f}")
    # Nothing moving would also pass if the constraint never applied, so the gap must be real.
    gx0 = abs(B["p"][0] - A["p"][0])
    gui.check("distance_x: the points still have a real horizontal gap", gx0 > 1.0, f"gap {gx0:.6f}")
    # Driven with a typed value on a fresh pair: the pair just dimensioned already has its
    # horizontal distance, and the button rightly opens no field to dimension it twice.
    A, C = now(gui.sketch(), a, c)
    dy0 = C["p"][1] - A["p"][1]
    pick("distance_x", A["p"], C["p"], of=" for the typed gap")
    gui.press("Horizontal distance")
    gui.values(20)

    def holds(d, after):
        A, C = now(d, a, c)
        gx, gy = abs(C["p"][0] - A["p"][0]), C["p"][1] - A["p"][1]
        gui.check(f"distance_x: the horizontal gap is the typed 20{after}", near(gx, 20.0, 1e-6), f"gap {gx:.9f}")
        gui.check(f"distance_x: the vertical gap is untouched{after}", near(gy, dy0, 1e-6), f"{dy0:.9f} -> {gy:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("distance_x", cons, "distance_x", [a, b], what="the distance_x the shown value set")
    stored("distance_x", cons, "distance_x", [a, c], 20.0, what="the distance_x of the typed 20")


def equal_radii(rung, button, o, small, large, before=None):
    """Two circles of radii `small` and `large` in the cell centred on o, made equal by `button`,
    with `before(a, b)` run on them first in Constrain mode when it is given. The circles carry no
    typed radius: a typed radius is a driving Radius constraint, and equal radii on two circles
    driven to different radii cannot hold. Yields as a relation does, and returns the two circles
    as Constrain mode read them and the reopened sketch's constraints."""
    circle_at(o, (-17, 0), small)
    circle_at(o, (15, 0), large)
    d1 = yield
    a, b = in_cell(d1, o, ["circle", "circle"])
    if before is not None:
        before(a, b)
    gui.check(f"{rung}: the circles start unequal", not near(a["radius"], b["radius"], 1e-6),
              f"radii {a['radius']:.6f}, {b['radius']:.6f}")
    pick(rung, rim(a), rim(b))
    gui.press(button)

    def holds(d, after):
        A, B = now(d, a, b)
        gui.check(f"{rung}: the radii are equal{after}", near(A["radius"], B["radius"], 1e-9),
                  f"{A['radius']:.9f} and {B['radius']:.9f}")
        gui.check(f"{rung}: and not equal at zero{after}", A["radius"] > 1e-6, f"radius {A['radius']:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored(rung, cons, "equal_radius", [a, b])
    return a, b, cons


def rel_equal_radius(o):
    """The shared Equal length button on two circles means equal radius."""
    yield from equal_radii("equal_radius", "Equal length", o, 6.5, 12.5)


def rel_equal_radius_button(o):
    """The Equal radius button itself, after Angle on the same two circles: a circle has no
    direction, so there is no angle to measure or keep, and Angle stores nothing."""
    def angle(a, b):
        pick("type_guards", rim(a), rim(b))
        gui.press("Angle")
        # The press refuses two circles on the status line, where for two lines it opens its value field: once the
        # status line says why, the press has run, and whether a field opened is read once.
        why = "Angle applies between two lines"
        said = until(lambda: any(w.kind == "text" and why in w.label for w in gui.widgets()))
        gui.check(f"type_guards: Angle on two circles says \"{why}\"", said,
                  [w.label for w in gui.widgets() if w.kind == "text"][:8])
        field = gui.value_field() or gui.sketch()["editing"]
        gui.check("type_guards: Angle on two circles opened no value field", not field, f"field {field}")
        if gui.value_field() is not None:
            key("Escape")

    a, b, cons = yield from equal_radii("equal_radius_button", "Equal radius", o, 5.0, 11.0, before=angle)
    on = bound(cons, [a, b])
    gui.check("type_guards: Angle on two circles stored no constraint", on == ["equal_radius"],
              f"the constraints on the circles: {on}")


def rel_collinear(o):
    """Two offset lines brought onto one infinite line. Oblique on purpose: axis-aligned lines pick
    up an inferred Horizontal as they are drawn."""
    line_at(o, (-27, -6), (-5.4, -2.7))
    line_at(o, (5.4, 6.3), (27, 9.9))
    d1 = yield
    a, b = in_cell(d1, o, ["line", "line"])
    dy0 = abs(a["p0"][1] - b["p0"][1])
    gui.check("collinear: the lines start on different lines", dy0 > 1.0, f"dy {dy0:.6f}")
    pick("collinear", mid(a), mid(b))
    gui.press("Collinear")

    def holds(d, after):
        A, B = now(d, a, b)
        (ax, ay), (dx, dy) = A["p0"], (A["p1"][0] - A["p0"][0], A["p1"][1] - A["p0"][1])
        crosses = [dx * (q[1] - ay) - dy * (q[0] - ax) for q in (B["p0"], B["p1"])]
        gui.check(f"collinear: both ends of the second line lie on the first{after}",
                  all(abs(c) < 1e-6 for c in crosses), f"cross products {[round(c, 9) for c in crosses]}")
        # A line collapsed to a point is collinear with anything, so both have to survive.
        gui.check(f"collinear: neither line collapsed{after}", A["length"] > 1.0 and B["length"] > 1.0,
                  f"lengths {A['length']:.6f}, {B['length']:.6f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("collinear", cons, "collinear", [a, b])


def rel_coincident(o):
    """Coincident on two points joins them, after Horizontal on the first alone: a point has no
    direction to make horizontal, and a constraint stored anyway would be listed in the panel and
    never act."""
    point_at(o, (-13, -9))
    point_at(o, (11, 8))
    d1 = yield
    a, b = in_cell(d1, o, ["point", "point"])
    pick("type_guards", a["p"])
    gui.press("Horizontal")
    (A,) = now(gui.sketch(), a)
    gui.check("type_guards: Horizontal on a point moved nothing", math.dist(A["p"], a["p"]) < 1e-9,
              f"{a['p']} -> {A['p']}")
    gap0 = math.dist(a["p"], b["p"])
    gui.check("coincident: the points start apart", gap0 > 1.0, f"gap {gap0:.6f}")
    pick("coincident", a["p"], b["p"])
    gui.press("Coincident")

    def holds(d, after):
        A, B = now(d, a, b)
        gap = math.dist(A["p"], B["p"])
        gui.check(f"coincident: the points are joined{after}", gap < 1e-6, f"gap {gap:.9f}")

    holds(gui.sketch(), "")
    d2, cons = yield
    holds(d2, " after the round trip")
    stored("coincident", cons, "coincident", [a, b])
    on = bound(cons, [a])
    gui.check("type_guards: Horizontal on a point stored no constraint", on == ["coincident"],
              f"the constraints on the point: {on}")


# Each relation's cell, as (column, row) in cells from the origin's. Live Parallel is drawn last:
# it is pressed in the open sketch as soon as its lines are drawn, with every other cell's shapes
# already there.
RELATIONS = {"equal_length": ((-2, 2), rel_equal_length), "perpendicular": ((-1, 2), rel_perpendicular),
             "symmetric_v": ((0, 2), rel_symmetric_v), "parallel": ((1, 2), rel_parallel),
             "vertical": ((-2, 1), rel_vertical), "concentric": ((-1, 1), rel_concentric),
             "tangent": ((0, 1), rel_tangent), "midpoint": ((1, 1), rel_midpoint),
             "symmetric_line": ((2, 1), rel_symmetric_line),
             "radius": ((-2, 0), rel_radius), "diameter": ((-1, 0), rel_diameter),
             "symmetric_h": ((1, 0), rel_symmetric_h), "fix": ((2, 0), rel_fix),
             "distance_y": ((-2, -1), rel_distance_y), "equal_radius": ((-1, -1), rel_equal_radius),
             "collinear": ((0, -1), rel_collinear), "distance_x": ((1, -1), rel_distance_x),
             "coincident": ((2, -1), rel_coincident),
             "equal_radius_button": ((-2, -2), rel_equal_radius_button),
             "live_parallel": ((2, 2), rel_live_parallel)}


def fits_canvas():
    """Raise unless the relations grid lies in the middle 60% of the canvas each way, clear of the
    panels along its edges, where every cell can be clicked."""
    _, _, w, h = next(x for x in gui.widgets() if x.kind == "canvas" and x.name == CANVAS["name"]).rect
    for X in (-2.5 * COL, 2.5 * COL):
        for Y in (-2.5 * ROW, 2.5 * ROW):
            u, v = gui.px(X, Y)
            if not (0.2 * w <= u <= 0.8 * w and 0.2 * h <= v <= 0.8 * h):
                raise RuntimeError(f"the relations grid's corner ({X}, {Y}) mm is at ({u:.0f}, {v:.0f}) px, outside "
                                   f"the middle of the {w} x {h} canvas: enlarge the window")


def rung_relations():
    """Every constraint button pressed on shapes drawn for it, in one sketch committed and reopened
    once, each relation read back from the reopened sketch. Live Parallel is pressed in the open
    sketch; the rest in Constrain mode (K) on the committed one."""
    gui.enter_sketch()
    fits_canvas()
    steps = []
    for name, ((col, row), rel) in RELATIONS.items():
        g = rel((col * COL, row * ROW))
        next(g)  # draws; a shape that will not draw fails the rung
        steps.append((name, g))
    if gui.sketch()["tool"] != "select":
        key("Escape")
    d1 = gui.constrain()
    pressed = []
    for name, g in steps:
        try:
            g.send(d1)
            pressed.append((name, g))
        except Exception as e:  # this relation fails, and the next one is still pressed
            gui.check(name, False, f"{type(e).__name__}: {e}")
            if gui.value_field() is not None:
                key("Escape")
    left = gui.sketch()
    d2 = confirm_and_reopen()
    kinds = [e["type"] for e in d2["entities"]]
    if kinds != [e["type"] for e in left["entities"]]:
        raise AssertionError(f"the reopened sketch's entities are not the ones Constrain mode left: {kinds}")
    cons = gui.cad("sketch_list_constraints")["items"]
    for name, g in pressed:
        try:
            g.send((d2, cons))
        except StopIteration:
            pass
        except Exception as e:
            gui.check(name, False, f"{type(e).__name__}: {e}")
    want = sum(DOF.values())
    gui.check(f"relations: the reopened sketch solves with {want} degrees of freedom",
              d2["solve_ok"] and d2["dof"] == want, f"solve_ok {d2['solve_ok']}, dof {d2['dof']}")
    leave()


# ---------------------------------------------------------------- history, files and size

def features():
    return gui.cad("design_describe")["features"]


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
    # Undo removes the shape as Delete does, which solves the sketch again for its degrees of
    # freedom. The solves while drawing leave each constraint satisfied only to within SolveSpace's
    # convergence tolerance (1e-8 mm), with a residual that differs by compiler and platform, and the
    # new solve may close it. So a survivor may move by that much and no more.
    shifts = [(math.dist(a["p0"], b["p0"]), math.dist(a["p1"], b["p1"])) for a, b in zip(before, after)]
    gui.check("undo: the survivors stay where they were", all(max(s) <= 1e-8 for s in shifts),
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
    person would type it."""
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
        gui.client.call("dialog_auto_answer",
                        rules=[{"match": "Save file as", "button": "ok", "path": path, "overwrite": True}])
        unanswered(gui.menu("File>Save Project as..."), "saving")
        size = os.path.getsize(path) if os.path.isfile(path) else 0
        gui.check("roundtrip: the project was written", size > 0, f"{size} bytes at {path}")
        gui.new_design()  # empty the tree, then read it back from the file
        # Opening asks first whether to save the current project when it has unsaved changes. No:
        # saving would write the emptied design over the file about to be opened.
        gui.client.call("dialog_auto_answer",
                        rules=[{"match": "Choose one file", "button": "ok", "path": path},
                               {"match": "unsaved changes", "button": "no"}])
        unanswered(gui.menu("File>Open Project..."), "opening")
    finally:
        gui.client.call("dialog_auto_answer", rules=[])
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
    # tests. It is the regression test for j7gc: a bulk sketch_draw made while a creation tool is
    # armed, here the rectangle, was read as a drawn gesture, opened that tool's value field and
    # swallowed the next key and click until an Escape dismissed it. So no Escape follows: the
    # gesture below has to land on the first try. Where the plate comes from is not under test,
    # only that drawing still works with it on screen.
    t0 = time.monotonic()
    gui.cad("sketch_draw", entities=ents)
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
         "dimension": rung_dimension, "relations": rung_relations, "badge": rung_badge,
         "undo": rung_undo, "feature_undo": rung_feature_undo,
         "roundtrip": rung_roundtrip, "scale": rung_scale}


def main():
    global gui
    # The Design tab's first showing, checked before anything builds the tab, which needs an app that has not: the
    # sweep starts this test in a new app. Then the baseline every test starts from, and the Design tab's own start.
    gui = CadGui(orca_mcp.session_client()[0])
    gui.start(first_showing=True)
    return gui.run({**RUNGS, "selfcheck-recovery": selfcheck_recovery}, sys.argv[1:] or list(RUNGS))


if __name__ == "__main__":
    sys.exit(main())
