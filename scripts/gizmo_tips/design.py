"""The Design tab's solid-modelling tooltips (DesignPanel.cpp): one picture per feature and command, and
one per value of the feature cards' options that is a shape or a choice.

The same plater view, grey model on the dark bed and one accent colour as the gizmo toolbar's set: the
accent marks what the feature made or acts on - the extruded solid, the rounded edge, the drilled
hole, the chosen plane. A sketch profile the feature starts from is drawn as an accent outline on its
plane. The sketch tools and the constraints are drawn flat, in design_2d.py.

The file names of the features follow the toolbar icons (design_extrude.svg -> tool_extrude.gif);
features that share an icon with a sketch tool get a name of their own (tool_mirror_body).
"""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "design_tips"  # resources/images/<OUT>

LINE = (60, 205, 185)  # an accent outline drawn over the picture: a sketch profile, a picked edge
LINE_SOFT = (60, 205, 185, 150)
PLANE_FILL = (26, 153, 138, 70)
HIDDEN = (200, 206, 206, 120)
WHITE = (235, 242, 242)

N_ANIM = 18


# --- shapes ----------------------------------------------------------------------------------------


def prism(p, verts, z0, z1):
    """A 2D polygon in x, y extruded from z0 to z1."""
    return extrude(sd_polygon2(p[:, 0], p[:, 1], verts), z0, z1)(p)


def frame(lo, hi):
    """The twelve edges of an axis-aligned box, as point pairs."""
    lo, hi = np.asarray(lo, float), np.asarray(hi, float)
    c = [np.array([x, y, z]) for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
    return [(a, b) for i, a in enumerate(c) for b in c[i + 1:] if np.sum(np.abs(a - b) > 1e-9) == 1]


def quad(o, pts, fill=PLANE_FILL, outline=LINE, width=1.4):
    o.draw.polygon([o.pt(p) for p in pts], fill=fill)
    o.polyline(pts, outline, width=width, closed=True)


def square_xy(c, h, z):
    return [np.array([c[0] + sx * h, c[1] + sy * h, z]) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]


def ring_pts(c, r, z, a0=0.0, a1=2 * math.pi, n=48):
    return [np.array([c[0] + r * math.cos(a), c[1] + r * math.sin(a), z]) for a in np.linspace(a0, a1, n)]


def wedge(x, y, a0, a1):
    """Inside (negative) for points whose angle about the z axis lies between a0 and a1 (radians)."""
    u0 = (math.cos(a0), math.sin(a0))
    u1 = (math.cos(a1), math.sin(a1))
    h0 = -(u0[0] * y - u0[1] * x)
    h1 = -(x * u1[1] - y * u1[0])
    return np.maximum(h0, h1) if (a1 - a0) <= math.pi else np.minimum(h0, h1)


def cam(size, target=(0.0, 0.0, 0.4), extent=2.6, **kw):
    return Camera(size, target=target, extent=extent, **kw)


def shot(scene, c):
    return Overlay(render(scene, c), c)


def timeline(k, start, length):
    return smooth(phase(k, start, length))


# --- features --------------------------------------------------------------------------------------

L_PROFILE = [(-0.6, -0.45), (0.6, -0.45), (0.6, -0.05), (-0.1, -0.05), (-0.1, 0.45), (-0.6, 0.45)]


def tool_extrude(k, n):
    h = 0.85 * timeline(k, 4, 9)

    def scene(p):
        items = [bed(p)]
        if h > 0.01:
            items.append((prism(p, L_PROFILE, 0.0, h), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE)
    o = shot(scene, c)
    o.polyline([np.array([x, y, 0.002]) for x, y in L_PROFILE], LINE, width=2.0, closed=True)
    if k < 4:
        o.pointer(np.array([-0.35, 0.2, 0.0]))
    elif h < 0.84:
        top = np.array([-0.35, 0.2, h])
        o.arrow(top, top + np.array([0.0, 0.0, 0.35]), WHITE, width=2.0)
    return o.finish(GIF_SIZE)


REV_PROFILE = [(0.25, 0.0), (0.7, 0.0), (0.7, 0.18), (0.42, 0.3), (0.42, 0.85), (0.25, 0.85)]
REV_START = math.atan2(-math.cos(math.radians(35.0)), math.sin(math.radians(35.0)))  # the plane facing the camera


def _revolved(p, profile, angle, start=REV_START):
    r = np.hypot(p[:, 0], p[:, 1])
    d = sd_polygon2(r, p[:, 2], profile)
    if angle >= 2 * math.pi - 1e-3:
        return d
    # Turn the points so the profile's plane is at angle 0.
    cs, sn = math.cos(-start), math.sin(-start)
    x = p[:, 0] * cs - p[:, 1] * sn
    y = p[:, 0] * sn + p[:, 1] * cs
    return np.maximum(d, wedge(x, y, 0.0, angle))


def tool_revolve(k, n):
    a = 2 * math.pi * timeline(k, 3, 11)

    def scene(p):
        items = [bed(p)]
        if a > 0.02:
            items.append((_revolved(p, REV_PROFILE, a), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=2.4)
    o = shot(scene, c)
    u = np.array([math.cos(REV_START), math.sin(REV_START), 0.0])
    o.line(np.array([0, 0, -0.1]), np.array([0, 0, 1.2]), WHITE, width=1.4)
    if a < 2 * math.pi - 1e-3:
        o.polyline([u * r + np.array([0, 0, z]) for r, z in REV_PROFILE], LINE, width=2.0, closed=True)
    return o.finish(GIF_SIZE)


SWEEP_C, SWEEP_R, SWEEP_T = np.array([-0.55, 0.0, 1.0]), 0.85, 0.16


def _swept(p, a1):
    q = p - SWEEP_C
    d = np.hypot(np.hypot(q[:, 0], q[:, 2]) - SWEEP_R, q[:, 1]) - SWEEP_T
    a0 = -math.pi / 2
    return np.maximum(d, wedge(q[:, 0], q[:, 2], a0, a1))


def tool_sweep(k, n):
    s = timeline(k, 3, 11)
    a1 = -math.pi / 2 + (math.pi / 2) * s

    def scene(p):
        items = [bed(p)]
        if s > 0.02:
            items.append((_swept(p, a1), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(-0.1, 0.0, 0.55), extent=2.5)
    o = shot(scene, c)
    path = [SWEEP_C + SWEEP_R * np.array([math.cos(t), 0.0, math.sin(t)]) for t in np.linspace(-math.pi / 2, 0.0, 32)]
    o.polyline(path, LINE_SOFT, width=1.4)
    start = SWEEP_C + np.array([0.0, 0.0, -SWEEP_R])
    o.polyline([start + SWEEP_T * np.array([0.0, math.cos(t), math.sin(t)]) for t in np.linspace(0, 2 * math.pi, 40)], LINE, width=2.0)
    return o.finish(GIF_SIZE)


LOFT_H = 1.0


def _loft(p, top, smooth_sections=True, mid=None):
    sq = np.maximum(np.abs(p[:, 0]), np.abs(p[:, 1])) - 0.5
    circ = np.hypot(p[:, 0], p[:, 1]) - 0.3
    t = np.clip(p[:, 2] / LOFT_H, 0.0, 1.0)
    d2 = (1 - t) * sq + t * circ
    return np.maximum.reduce([d2 * 0.8, -p[:, 2], p[:, 2] - top])


def tool_loft(k, n):
    s = timeline(k, 3, 11)

    def scene(p):
        items = [bed(p)]
        if s > 0.02:
            items.append((_loft(p, LOFT_H * s), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.3)
    o = shot(scene, c)
    o.polyline(square_xy((0, 0), 0.5, 0.002), LINE, width=2.0, closed=True)
    o.polyline(ring_pts((0, 0), 0.3, LOFT_H), LINE, width=2.0)
    return o.finish(GIF_SIZE)


BOX_H = np.array([0.65, 0.42, 0.38])
BOX_C = np.array([0.0, 0.0, 0.38])


def _box(p, c=BOX_C, h=BOX_H):
    return sd_box(p, c, h, 0.0)


def _filleted(p, r):
    """The box with its top front edge (along x) rounded to radius r; the second value marks the round."""
    d = _box(p)
    if r <= 1e-3:
        return d, np.zeros(len(p), bool)
    qy = (-BOX_H[1] + r) - p[:, 1]
    qz = p[:, 2] - (2 * BOX_H[2] - r)
    corner = (qy > 0) & (qz > 0)
    d = np.where(corner, np.maximum(d, np.hypot(qy, qz) - r), d)
    return d, corner


def _chamfered(p, c):
    d = _box(p)
    if c <= 1e-3:
        return d, np.zeros(len(p), bool)
    cut = ((-p[:, 1]) + p[:, 2] - (BOX_H[1] + 2 * BOX_H[2] - c)) / math.sqrt(2.0)
    return np.maximum(d, cut), cut > -0.01


EDGE_A = np.array([-BOX_H[0], -BOX_H[1], 2 * BOX_H[2]])
EDGE_B = np.array([BOX_H[0], -BOX_H[1], 2 * BOX_H[2]])


def _dressup(k, size, shape, amount):
    def scene(p):
        d, mark = shape(p, amount)
        return union(bed(p), (d, np.where(mark, M_ACCENT, M_PART)))

    c = cam(size, target=(0.0, 0.0, 0.35), extent=2.2)
    return shot(scene, c)


def tool_dressup(k, n):
    r = 0.3 * timeline(k, 5, 8)
    o = _dressup(k, GIF_SIZE, _filleted, r)
    if r < 0.01:
        o.line(EDGE_A, EDGE_B, LINE, width=3.0 if k >= 2 else 1.0)
    if k < 5:
        o.pointer(0.5 * (EDGE_A + EDGE_B))
    return o.finish(GIF_SIZE)


def dressup_fillet(k, n):
    return _dressup(k, SIZE, _filleted, 0.28).finish(SIZE)


def dressup_chamfer(k, n):
    return _dressup(k, SIZE, _chamfered, 0.3).finish(SIZE)


def tool_draft(k, n):
    a = math.radians(14.0) * timeline(k, 4, 9)
    nrm = np.array([0.0, -math.cos(a), math.sin(a)])
    p0 = np.array([0.0, -BOX_H[1], 0.0])

    def scene(p):
        face = (p - p0) @ nrm
        d = np.maximum(_box(p), face)
        return union(bed(p), (d, np.where(face > -0.01, M_ACCENT, M_PART)))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.2)
    o = shot(scene, c)
    if k < 4:
        o.pointer(np.array([0.1, -BOX_H[1], 0.35]))
    return o.finish(GIF_SIZE)


def tool_shell(k, n):
    s = timeline(k, 4, 9)
    t = 0.07
    top = 2 * BOX_H[2]

    def scene(p):
        d = _box(p)
        mat = np.where(p[:, 2] > top - 0.01, M_ACCENT, M_PART) if k >= 2 else np.full(len(p), M_PART)
        if s > 0.01:
            floor = top - (top - t) * s
            inner = sd_box(p, (0.0, 0.0, 0.5 * (floor + top + 0.5)), (BOX_H[0] - t, BOX_H[1] - t, 0.5 * (top + 0.5 - floor)))
            d = np.maximum(d, -inner)
            mat = np.where(np.abs(inner) < 0.01, M_ACCENT, M_PART)
        return union(bed(p), (d, mat))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.2, el=42.0)
    o = shot(scene, c)
    if k < 4:
        o.pointer(np.array([0.1, 0.0, top]))
    return o.finish(GIF_SIZE)


HOLE_C = np.array([0.1, -0.05])


def _drilled(p, depth, r=0.17, section=False):
    top = 2 * BOX_H[2]
    d = _box(p)
    hole = sd_cylinder(p, HOLE_C, r, top - depth, top + 0.5)
    d = np.maximum(d, -hole)
    mat = np.where(np.abs(hole) < 0.01, M_ACCENT, M_PART)
    if section:  # the front half cut away, so the depth shows
        cut = HOLE_C[1] - p[:, 1]
        d = np.maximum(d, cut)
        mat = np.where(cut > -0.01, M_PART2, mat)
    return d, mat


def tool_hole(k, n):
    depth = 0.78 * timeline(k, 4, 9)
    top = 2 * BOX_H[2]

    def scene(p):
        if depth < 0.01:
            return union(bed(p), (_box(p), M_PART))
        return union(bed(p), _drilled(p, depth))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.2, el=45.0)
    o = shot(scene, c)
    if depth < 0.01:
        o.circle_on(np.array([HOLE_C[0], HOLE_C[1], top]), (0, 0, 1), 0.17, LINE, width=2.0)
    if k < 4:
        o.pointer(np.array([HOLE_C[0], HOLE_C[1], top]))
    return o.finish(GIF_SIZE)


def _hole_still(depth):
    def scene(p):
        return union(bed(p), _drilled(p, depth, section=True))
    return shot(scene, cam(SIZE, target=(0.0, 0.0, 0.35), extent=2.2, el=22.0)).finish(SIZE)


def hole_blind(k, n):
    return _hole_still(0.45)


def hole_through(k, n):
    return _hole_still(1.2)


def _thread_r(p, r, depth, pitch, z0, z1):
    rad = np.hypot(p[:, 0], p[:, 1])
    th = np.arctan2(p[:, 1], p[:, 0])
    ph = p[:, 2] / pitch - th / (2 * math.pi)
    g = np.abs((ph % 1.0) - 0.5) * 2.0  # 0 at the crest, 1 in the root
    inside = (p[:, 2] > z0) & (p[:, 2] < z1)
    return rad, np.where(inside, depth * g, 0.0)


def _rod(p):
    rad, groove = _thread_r(p, 0.3, 0.07, 0.16, 0.25, 1.4)
    d = np.maximum(rad - 0.3 + groove, np.abs(p[:, 2] - 0.75) - 0.75) * 0.6
    head = sd_box(p, (0, 0, 0.12), (0.5, 0.5, 0.12), 0.02)
    mat = np.where((p[:, 2] > 0.26), M_ACCENT, M_PART)
    return np.minimum(d, head), np.where(head < d, M_PART, mat)


def tool_thread(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.7), extent=2.2)
    return shot(lambda p: union(bed(p), _rod(p)), c).finish(SIZE)


def thread_external(k, n):
    return tool_thread(k, n)


def thread_internal(k, n):
    def scene(p):
        rad, groove = _thread_r(p, 0.35, 0.07, 0.16, -1.0, 2.0)
        hole = (rad - 0.33 - groove) * 0.6
        block = sd_box(p, (0, 0, 0.45), (0.75, 0.75, 0.45))
        cut = -p[:, 1]
        d = np.maximum.reduce([block, -hole, cut])
        mat = np.where(cut > -0.01, M_PART2, np.where(np.abs(hole) < 0.02, M_ACCENT, M_PART))
        return union(bed(p), (d, mat))
    c = cam(SIZE, target=(0.0, 0.2, 0.45), extent=2.3, el=20.0)
    return shot(scene, c).finish(SIZE)


def _plate_bosses(p, centres, count, plate):
    items = [plate(p)]
    for i, (x, y) in enumerate(centres[:count]):
        items.append((sd_cylinder(p, (x, y), 0.14, 0.0, 0.5, 0.02), M_PART if i == 0 else M_ACCENT))
    return union(*items)


LIN = [(-0.75 + 0.5 * i, 0.0) for i in range(4)]
CIRC = [(0.55 * math.cos(math.radians(90 + 60 * i)), 0.55 * math.sin(math.radians(90 + 60 * i))) for i in range(6)]


def _lin_plate(p):
    return sd_box(p, (0, 0, 0.06), (1.05, 0.35, 0.06), 0.02), M_PART


def _circ_plate(p):
    return sd_cylinder(p, (0, 0), 0.85, 0.0, 0.12, 0.02), M_PART


def tool_pattern(k, n):
    count = 1 + min(3, max(0, (k - 3) // 3))
    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.2), extent=2.6)
    return shot(lambda p: union(bed(p), _plate_bosses(p, LIN, count, _lin_plate)), c).finish(GIF_SIZE)


def pattern_linear(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.2), extent=2.6)
    return shot(lambda p: union(bed(p), _plate_bosses(p, LIN, 4, _lin_plate)), c).finish(SIZE)


def pattern_circular(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.2), extent=2.3, el=45.0)
    return shot(lambda p: union(bed(p), _plate_bosses(p, CIRC, 6, _circ_plate)), c).finish(SIZE)


# The four results of a feature against the body it touches: the block is what was there, the
# cylinder the new shape.
def _block(p):
    return sd_box(p, (-0.2, 0.05, 0.35), (0.55, 0.45, 0.35), 0.01)


def _cyl(p):
    return sd_cylinder(p, (0.3, -0.2), 0.33, 0.0, 0.95, 0.01)


def _result(kind):
    def scene(p):
        a, b = _block(p), _cyl(p)
        if kind == "new":
            body = union((a, M_PART2), (b, M_ACCENT))
        elif kind == "join":
            body = union((a, M_PART), (b, M_ACCENT))
        elif kind == "cut":
            body = (np.maximum(a, -b), np.where(-b > a - 0.005, M_ACCENT, M_PART))
        elif kind == "intersect":
            body = (np.maximum(a, b), np.where(b > a - 0.005, M_ACCENT, M_PART))
        elif kind == "target":
            body = union((a, M_ACCENT), (b, M_PART2))
        else:  # tool
            body = union((a, M_PART2), (b, M_ACCENT))
        return union(bed(p), body)

    c = cam(SIZE, target=(0.05, -0.05, 0.4), extent=2.3)
    o = shot(scene, c)
    if kind == "new":  # the new body goes on inside the old one: show it continuing
        o.polyline(ring_pts((0.3, -0.2), 0.33, 0.0, math.radians(100), math.radians(330)), HIDDEN, width=1.2)
    return o.finish(SIZE)


def result_new(k, n):
    return _result("new")


def result_join(k, n):
    return _result("join")


def result_cut(k, n):
    return _result("cut")


def result_intersect(k, n):
    return _result("intersect")


def bool_target(k, n):
    return _result("target")


def bool_tool(k, n):
    return _result("tool")


def tool_boolean(k, n):
    return _result("cut")


def _keep(kept):
    def scene(p):
        a, b = _block(p), _cyl(p)
        items = [bed(p), (np.maximum(a, -(b - 0.004)), np.where(-b > a - 0.01, M_ACCENT, M_PART))]
        if kept:
            items.append((sd_cylinder(p, (0.3, -0.2), 0.33, 0.0, 0.95, 0.01) + 0.006, M_PART2))
        return union(*items)
    return shot(scene, cam(SIZE, target=(0.05, -0.05, 0.4), extent=2.3)).finish(SIZE)


def keep_tool_off(k, n):
    return _keep(False)


def keep_tool_on(k, n):
    return _keep(True)


def tool_cut(k, n):
    split = timeline(k, 7, 7)
    off = np.array([0.35, 0.0, 0.0]) * split
    xc = 0.05

    def scene(p):
        body = lambda q: sd_box(q, (0, 0, 0.42), (0.7, 0.45, 0.42), 0.02)
        if split <= 0.0:
            return union(bed(p), (body(p), M_PART))
        left = np.maximum(body(p + off), (p[:, 0] + off[0]) - xc)
        right = np.maximum(body(p - off), xc - (p[:, 0] - off[0]))
        return union(bed(p), (left, np.where(p[:, 0] + off[0] > xc - 0.01, M_ACCENT, M_PART)),
                     (right, np.where(p[:, 0] - off[0] < xc + 0.01, M_ACCENT, M_PART)))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.4), extent=2.7)
    o = shot(scene, c)
    alpha = timeline(k, 0, 4) * (1.0 - split)
    if alpha > 0.0:
        pts = [np.array([xc, y, z]) for y, z in ((-0.75, -0.05), (0.75, -0.05), (0.75, 1.1), (-0.75, 1.1))]
        quad(o, pts, (26, 153, 138, int(80 * alpha)), (60, 205, 185, int(230 * alpha)))
    return o.finish(GIF_SIZE)


def tool_plane(k, n):
    return _plane_kind("offset")


def tool_new_sketch(k, n):
    top = 2 * BOX_H[2]
    picked = k >= 3
    s = timeline(k, 5, 8)

    def scene(p):
        return union(bed(p), (_box(p), np.where(picked & (p[:, 2] > top - 0.01), M_ACCENT, M_PART)))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.2, el=40.0)
    o = shot(scene, c)
    if s > 0.0:
        a = np.array([-0.4, -0.25, top + 0.002])
        b = a + np.array([0.75, 0.5, 0.0]) * s
        o.polyline([a, np.array([b[0], a[1], top]), b, np.array([a[0], b[1], top])], WHITE, width=2.2, closed=True)
    if k < 5:
        o.pointer(np.array([0.2, 0.1, top]))
    elif s < 1.0:
        o.pointer(np.array([-0.4 + 0.75 * s, -0.25 + 0.5 * s, top]))
    return o.finish(GIF_SIZE)


def _bracket(p):
    base = sd_box(p, (0, 0, 0.1), (0.8, 0.45, 0.1), 0.02)
    wall = sd_box(p, (-0.7, 0, 0.55), (0.1, 0.45, 0.55), 0.02)
    holes = np.minimum(sd_cylinder(p, (0.35, -0.15), 0.12, -1, 1), sd_cylinder(p, (0.35, 0.2), 0.12, -1, 1))
    bore = np.hypot(p[:, 1], p[:, 2] - 0.75) - 0.16
    return np.maximum(np.maximum(np.minimum(base, wall), -holes), -np.where(p[:, 0] < -0.5, bore, 9.0))


def tool_step(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.4), extent=2.4)
    return shot(lambda p: union(bed(p), (_bracket(p), M_ACCENT)), c).finish(SIZE)


def tool_import_mesh(k, n):
    facets = [(0.42 * math.cos(2 * math.pi * i / 9), 0.42 * math.sin(2 * math.pi * i / 9)) for i in range(9)]

    def scene(p):
        mesh = prism(p - np.array([0.65, -0.45, 0.0]), facets, 0.0, 0.85)
        solid = sd_cylinder(p, (-0.45, 0.55), 0.42, 0.0, 0.85, 0.02)
        return union(bed(p), (mesh, M_PART), (solid, M_ACCENT))

    c = cam(SIZE, target=(0.0, 0.0, 0.4), extent=2.6)
    o = shot(scene, c)
    o.arrow(np.array([0.35, -0.05, 0.45]), np.array([-0.05, 0.3, 0.45]), WHITE, width=2.0)
    return o.finish(SIZE)


L_BODY = [(-0.9, -0.4), (-0.25, -0.4), (-0.25, -0.05), (-0.55, 0.4), (-0.9, 0.4)]


def tool_mirror_body(k, n):
    s = timeline(k, 4, 10)

    def scene(p):
        items = [bed(p), (prism(p, L_BODY, 0.0, 0.5), M_PART)]
        if s > 0.03:
            q = p.copy()
            q[:, 0] = -q[:, 0] / s
            items.append((prism(q, L_BODY, 0.0, 0.5) * s, M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=2.7)
    o = shot(scene, c)
    quad(o, [np.array([0.0, y, z]) for y, z in ((-0.7, -0.02), (0.7, -0.02), (0.7, 0.85), (-0.7, 0.85))])
    return o.finish(GIF_SIZE)


def tool_move_body(k, n):
    s = timeline(k, 3, 11)
    off = np.array([0.55, 0.35, 0.0]) * s
    R = rot_z(math.radians(60.0) * s)
    h = np.array([0.4, 0.25, 0.25])
    c0 = np.array([-0.35, -0.2, 0.25])

    def scene(p):
        q = (p - c0 - off) @ R
        return union(bed(p), (sd_box(q, (0, 0, 0), h, 0.02), M_ACCENT))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=2.6)
    o = shot(scene, c)
    for a, b in frame(c0 - h, c0 + h):
        o.line(a, b, HIDDEN, width=1.0)
    return o.finish(GIF_SIZE)


def tool_delete_face(k, n):
    top = 2 * BOX_H[2]
    depth = 0.4 * (1.0 - timeline(k, 7, 7))
    picked = k >= 3

    def scene(p):
        d = _box(p)
        mat = np.full(len(p), M_PART)
        if depth > 0.01:
            pocket = sd_box(p, (0.0, 0.0, top), (0.35, 0.2, depth))
            d = np.maximum(d, -pocket)
            if picked:
                mat = np.where(np.abs(pocket) < 0.01, M_ACCENT, M_PART)
        return union(bed(p), (d, mat))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.2, el=45.0)
    o = shot(scene, c)
    if k < 4:
        o.pointer(np.array([0.1, 0.05, top - 0.3]))
    return o.finish(GIF_SIZE)


def _triad(o, at, size=0.38, x=(1, 0, 0), y=(0, 1, 0), z=(0, 0, 1)):
    at = np.asarray(at, float)
    o.arrow(at, at + size * np.asarray(x, float), AXIS_X, width=2.0, head=7.0)
    o.arrow(at, at + size * np.asarray(y, float), AXIS_Y, width=2.0, head=7.0)
    o.arrow(at, at + size * np.asarray(z, float), AXIS_Z, width=2.0, head=7.0)


def _mate_scene(p):
    a = sd_box(p, (0, 0, 0.25), (0.7, 0.5, 0.25), 0.02)
    b = sd_box(p, (0.1, 0.0, 0.7), (0.35, 0.3, 0.2), 0.02)
    return union(bed(p), (a, M_PART), (b, M_ACCENT))


def tool_mate(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.45), extent=2.3)
    o = shot(_mate_scene, c)
    _triad(o, (0.1, 0.0, 0.5))
    return o.finish(SIZE)


def _mate_kind(kind):
    c = cam(SIZE, target=(0.0, 0.0, 0.45), extent=2.3)
    o = shot(_mate_scene, c)
    top = np.array([0.1, 0.0, 0.9])
    if kind == "fastened":
        _triad(o, (0.1, 0.0, 0.5))
    if kind in ("planar",):
        o.arrow(top, top + np.array([0.45, 0, 0]), WHITE, 2.2)
        o.arrow(top, top + np.array([0, 0.45, 0]), WHITE, 2.2)
    if kind in ("revolute", "cylindrical"):
        o.polyline(ring_pts((0.1, 0.0), 0.5, 0.95, 0.0, 1.6 * math.pi), WHITE, width=2.0)
        o.line(np.array([0.1, 0, 0.5]), np.array([0.1, 0, 1.35]), WHITE, width=1.2)
    if kind in ("slider", "cylindrical"):
        o.arrow(top, top + np.array([0, 0, 0.45]), WHITE, 2.2)
    return o.finish(SIZE)


def mate_fastened(k, n):
    return _mate_kind("fastened")


def mate_planar(k, n):
    return _mate_kind("planar")


def mate_revolute(k, n):
    return _mate_kind("revolute")


def mate_slider(k, n):
    return _mate_kind("slider")


def mate_cylindrical(k, n):
    return _mate_kind("cylindrical")


def _helix(p, R=0.45, pitch=0.3, t=0.06, z0=0.06, z1=1.2):
    rad = np.hypot(p[:, 0], p[:, 1])
    th = np.arctan2(p[:, 1], p[:, 0])
    ph = (p[:, 2] / pitch - th / (2 * math.pi))
    w = ((ph + 0.5) % 1.0 - 0.5) * pitch
    d = np.hypot(rad - R, w) - t
    return np.maximum(d * 0.7, np.abs(p[:, 2] - 0.5 * (z0 + z1)) - 0.5 * (z1 - z0))


def tool_helix(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.6), extent=2.2)
    return shot(lambda p: union(bed(p), (_helix(p), M_ACCENT)), c).finish(SIZE)


def tool_axis(k, n):
    return _axis_kind("cylinder")


def tool_coordsys(k, n):
    c = cam(SIZE, target=(0.0, 0.0, 0.35), extent=2.2)
    o = shot(lambda p: union(bed(p), (_box(p), M_PART)), c)
    _triad(o, (-BOX_H[0] + 0.05, -BOX_H[1] + 0.05, 2 * BOX_H[2]), 0.5)
    return o.finish(SIZE)


def tool_project(k, n):
    def scene(p):
        body = np.minimum(sd_cylinder(p, (0.1, 0.1), 0.35, 0.7, 1.1, 0.02), sd_box(p, (0.1, 0.1, 0.78), (0.6, 0.18, 0.08), 0.01))
        return union(bed(p), (body, M_PART))

    c = cam(SIZE, target=(0.0, 0.0, 0.5), extent=2.6)
    o = shot(scene, c)
    o.polyline(ring_pts((0.1, 0.1), 0.35, 0.003), LINE, width=2.2)
    o.polyline([np.array([0.1 + x, 0.1 + y, 0.003]) for x, y in ((-0.6, -0.18), (0.6, -0.18), (0.6, 0.18), (-0.6, 0.18))],
               LINE, width=2.2, closed=True)
    for x, y in ((-0.5, -0.08), (0.7, -0.08)):
        o.line(np.array([x, y, 0.7]), np.array([x, y, 0.01]), HIDDEN, width=1.0)
    return o.finish(SIZE)


# --- surfaces: sheets without thickness, drawn as thin shells --------------------------------------

SHEET = 0.018


def _wave(x, z):
    return 0.25 * np.sin(2.2 * x) + 0.08 * z


def tool_surface_extrude(k, n):
    def scene(p):
        d = np.maximum.reduce([np.abs(p[:, 1] - _wave(p[:, 0], 0)) * 0.8 - SHEET, np.abs(p[:, 0]) - 0.8, np.abs(p[:, 2] - 0.45) - 0.45])
        return union(bed(p), (d, M_ACCENT))
    c = cam(SIZE, target=(0.0, 0.0, 0.45), extent=2.4)
    o = shot(scene, c)
    o.polyline([np.array([x, float(_wave(np.array(x), 0)), 0.004]) for x in np.linspace(-0.8, 0.8, 30)], WHITE, width=2.0)
    return o.finish(SIZE)


VASE = [(0.35, 0.0), (0.55, 0.25), (0.5, 0.55), (0.3, 0.8), (0.38, 1.05)]


def _vase(p):
    r = np.hypot(p[:, 0], p[:, 1])
    d = np.min(np.stack([sd_segment2(r, p[:, 2], a, b) for a, b in zip(VASE[:-1], VASE[1:])]), axis=0)
    return d - SHEET


def tool_surface_revolve(k, n):
    def scene(p):
        return union(bed(p), (np.maximum(_vase(p), wedge(p[:, 0], p[:, 1], -math.pi * 0.15, math.pi * 1.45)), M_ACCENT))
    c = cam(SIZE, target=(0.0, 0.0, 0.5), extent=2.2)
    return shot(scene, c).finish(SIZE)


def tool_surface_loft(k, n):
    def f(x, z):
        t = np.clip(z, 0.0, 1.0)
        return (1 - t) * 0.3 * np.sin(2.5 * x) - t * 0.3 * np.sin(2.5 * x + 0.8)

    def scene(p):
        d = np.maximum.reduce([np.abs(p[:, 1] - f(p[:, 0], p[:, 2])) * 0.6 - SHEET, np.abs(p[:, 0]) - 0.75, np.abs(p[:, 2] - 0.5) - 0.5])
        return union(bed(p), (d, M_ACCENT))
    c = cam(SIZE, target=(0.0, 0.0, 0.5), extent=2.4)
    o = shot(scene, c)
    xs = np.linspace(-0.75, 0.75, 30)
    o.polyline([np.array([x, float(f(np.array(x), 0.0)), 0.004]) for x in xs], WHITE, width=2.0)
    o.polyline([np.array([x, float(f(np.array(x), 1.0)), 1.0]) for x in xs], WHITE, width=2.0)
    return o.finish(SIZE)


def _dome(x, y, a=0.6):
    return 0.35 + 0.35 * np.clip(1 - (x / a) ** 2, 0, 1) * np.clip(1 - (y / a) ** 2, 0, 1)


def tool_surface(k, n):
    a = 0.6

    def scene(p):
        d = np.maximum(np.abs(p[:, 2] - _dome(p[:, 0], p[:, 1], a)) * 0.6 - SHEET, np.maximum(np.abs(p[:, 0]), np.abs(p[:, 1])) - a)
        return union(bed(p), (d, M_ACCENT))
    c = cam(SIZE, target=(0.0, 0.0, 0.4), extent=2.1)
    o = shot(scene, c)
    o.polyline(square_xy((0, 0), a, 0.35), WHITE, width=2.2, closed=True)
    return o.finish(SIZE)


def _sheet_wave(p, lift=0.0, thick=None):
    f = 0.35 + 0.15 * np.sin(2.5 * p[:, 0]) + lift
    d = np.abs(p[:, 2] - f) if thick is None else np.abs(p[:, 2] - f - thick / 2) - thick / 2 + SHEET
    return np.maximum(d * 0.7 - SHEET, np.maximum(np.abs(p[:, 0]) - 0.75, np.abs(p[:, 1]) - 0.5))


def tool_surface_offset(k, n):
    def scene(p):
        return union(bed(p), (_sheet_wave(p), M_PART), (_sheet_wave(p, 0.3), M_ACCENT))
    return shot(scene, cam(SIZE, target=(0.0, 0.0, 0.5), extent=2.2)).finish(SIZE)


def tool_thicken_surface(k, n):
    def scene(p):
        return union(bed(p), (_sheet_wave(p, 0.0, 0.18), M_ACCENT), (_sheet_wave(p, 0.18), M_PART))
    return shot(scene, cam(SIZE, target=(0.0, 0.0, 0.5), extent=2.2)).finish(SIZE)


def tool_thicken(k, n):
    top = 2 * BOX_H[2]
    s = timeline(k, 5, 8)

    def scene(p):
        items = [bed(p), (_box(p), np.where((k >= 2) & (p[:, 2] > top - 0.01), M_ACCENT, M_PART))]
        if s > 0.02:
            items.append((sd_box(p, (0, 0, top + 0.02 + 0.08 * s), (BOX_H[0], BOX_H[1], 0.08 * s), 0.0), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.4), extent=2.2)
    o = shot(scene, c)
    if k < 5:
        o.pointer(np.array([0.2, 0.1, top]))
    return o.finish(GIF_SIZE)


def tool_rib(k, n):
    s = timeline(k, 4, 10)
    # In the plane x = 0, as (y, z): the line's end on the plate, its end on the wall, and the corner.
    A, B, C = np.array([0.75, 0.15]), np.array([-0.45, 1.05]), np.array([-0.45, 0.15])
    poly = [tuple(A), tuple(B), tuple(B + (C - B) * s), tuple(A + (C - A) * s)]

    def scene(p):
        plate = sd_box(p, (0, 0.2, 0.075), (0.5, 0.65, 0.075), 0.01)
        wall = sd_box(p, (0, -0.52, 0.6), (0.5, 0.075, 0.6), 0.01)
        items = [bed(p), (np.minimum(plate, wall), M_PART)]
        if s > 0.02:
            q = p[:, [1, 2, 0]]
            items.append((extrude(sd_polygon2(q[:, 0], q[:, 1], poly), -0.05, 0.05)(q), M_ACCENT))
        return union(*items)

    c = cam(GIF_SIZE, target=(0.0, 0.1, 0.45), extent=2.4, az=60.0)
    o = shot(scene, c)
    o.line(np.array([0.0, A[0], A[1]]), np.array([0.0, B[0], B[1]]), LINE, width=2.2)
    return o.finish(GIF_SIZE)


def tool_place_on_face(k, n):
    half = np.array([0.55, 0.32, 0.22])
    R0 = rot_z(math.radians(20.0)) @ rot_y(math.radians(-40.0)) @ rot_x(math.radians(25.0))
    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=2.6)
    faces = [s * e for e in np.eye(3) for s in (1.0, -1.0)]
    visible = [f for f in faces if (R0 @ f) @ c.c > 0.15]
    face = min(visible, key=lambda f: (R0 @ f)[2])
    m = R0 @ face
    axis = np.cross(m, [0.0, 0.0, -1.0])
    angle = math.acos(np.clip(m @ np.array([0.0, 0.0, -1.0]), -1.0, 1.0))
    s = timeline(k, 5, 9)
    R = rot_axis(axis, angle * s) @ R0
    verts = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half
    ctr = np.array([0.0, 0.0, -(verts @ R.T)[:, 2].min()])
    along = np.abs(face) @ half

    def scene(p):
        q = (p - ctr) @ R
        mat = np.where((q @ face > along - 0.012) & (k >= 2), M_ACCENT, M_PART)
        return union(bed(p), (sd_box(q, (0, 0, 0), half, 0.02), mat))

    o = shot(scene, c)
    if k < 5:
        o.pointer(ctr + R @ (face * along))
    return o.finish(GIF_SIZE)


def _sectioned(p, xp, keep_low=True):
    body = sd_cylinder(p, (0, 0), 0.6, 0.0, 1.0, 0.02)
    bore = sd_cylinder(p, (0, 0), 0.3, 0.15, 1.5)
    cross = np.hypot(p[:, 1], p[:, 2] - 0.55) - 0.15
    d = np.maximum.reduce([body, -bore, -cross])
    cut = (p[:, 0] - xp) if keep_low else (xp - p[:, 0])
    return np.maximum(d, cut), np.where(cut > -0.01, M_ACCENT, M_PART)


def tool_section(k, n):
    xp = 0.75 - 0.75 * timeline(k, 2, 11)

    def scene(p):
        return union(bed(p), _sectioned(p, xp))

    c = cam(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.3)
    o = shot(scene, c)
    o.polyline([np.array([xp, y, z]) for y, z in ((-0.8, -0.02), (0.8, -0.02), (0.8, 1.15), (-0.8, 1.15))], LINE_SOFT, width=1.4, closed=True)
    return o.finish(GIF_SIZE)


def tool_flip_section(k, n):
    c = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=2.3, az=215.0)
    return shot(lambda p: union(bed(p), _sectioned(p, 0.0, keep_low=False)), c).finish(SIZE)


def _commit(assembly):
    a_lo, a_hi = np.array([-0.85, -0.35, 0.0]), np.array([-0.1, 0.35, 0.5])
    b_c, b_r, b_h = (0.4, 0.0), 0.3, 0.8

    def scene(p):
        return union(bed(p), (sd_box(p, 0.5 * (a_lo + a_hi), 0.5 * (a_hi - a_lo), 0.02), M_PART),
                     (sd_cylinder(p, b_c, b_r, 0.0, b_h, 0.02), M_PART2))

    c = cam(SIZE, target=(0.0, 0.0, 0.35), extent=2.5)
    o = shot(scene, c)
    boxes = [(np.array([-0.9, -0.4, 0.0]), np.array([0.75, 0.4, 0.85]))] if assembly else \
            [(a_lo - 0.05, a_hi + 0.05), (np.array([0.05, -0.35, 0.0]), np.array([0.75, 0.35, 0.85]))]
    for lo, hi in boxes:
        for a, b in frame(lo, hi):
            o.line(a, b, LINE, width=1.6)
    return o.finish(SIZE)


def commit_assembly(k, n):
    return _commit(True)


def commit_bodies(k, n):
    return _commit(False)


# --- extrude end conditions ----------------------------------------------------------------------

PROF_H = 0.32


def _end(kind):
    zs = {"blind": (0.0, 0.0, 0.8), "symmetric": (0.45, 0.05, 0.85), "two_sided": (0.4, 0.15, 1.0)}
    c = cam(SIZE, target=(0.0, 0.0, 0.45), extent=2.4)
    if kind in zs:
        z_sk, z0, z1 = zs[kind]

        def scene(p):
            return union(bed(p), (sd_box(p, (0, 0, 0.5 * (z0 + z1)), (PROF_H, PROF_H, 0.5 * (z1 - z0)), 0.0), M_ACCENT))
        o = shot(scene, c)
        if z_sk > 0.0:
            quad(o, square_xy((0, 0), 0.7, z_sk), (60, 66, 70, 110), (150, 156, 158), 1.0)
        o.polyline(square_xy((0, 0), PROF_H, z_sk + 0.003), WHITE, width=2.0, closed=True)
        return o.finish(SIZE)
    if kind == "through_all":
        def scene(p):
            block = sd_box(p, (0, 0, 0.3), (0.75, 0.6, 0.3), 0.01)
            hole = sd_box(p, (0, 0, 0.5), (PROF_H, PROF_H, 2.0))
            return union(bed(p), (np.maximum(block, -hole), np.where(np.abs(hole) < 0.01, M_ACCENT, M_PART)))
        o = shot(scene, cam(SIZE, target=(0.0, 0.0, 0.35), extent=2.3, el=45.0))
        o.polyline(square_xy((0, 0), PROF_H, 0.603), WHITE, width=2.0, closed=True)
        return o.finish(SIZE)
    if kind == "up_to_face":
        tilt = np.array([0.0, math.sin(math.radians(-18)), math.cos(math.radians(-18))])
        base = np.array([0.0, 0.0, 0.85])

        def scene(p):
            under = (p - base) @ tilt
            roof = np.maximum(sd_box(p, (0, 0.1, 0.95), (0.8, 0.75, 0.35)), np.abs(under - 0.06) - 0.06)
            post = np.maximum(sd_box(p, (0, 0, 0.6), (PROF_H, PROF_H, 0.6), 0.0), under)
            return union(bed(p), (roof, np.where(under < 0.01, M_ACCENT_UNDER, M_PART)), (post, M_ACCENT))
        o = shot(scene, c)
        o.polyline(square_xy((0, 0), PROF_H, 0.003), WHITE, width=2.0, closed=True)
        return o.finish(SIZE)
    # up to vertex
    vtx = np.array([0.95, 0.3, 0.6])

    def scene(p):
        other = sd_box(p, (0.95, 0.55, 0.3), (0.25, 0.25, 0.3), 0.0)
        post = sd_box(p, (-0.2, 0, 0.3), (PROF_H, PROF_H, 0.3), 0.0)
        return union(bed(p), (other, M_PART), (post, M_ACCENT))
    o = shot(scene, cam(SIZE, target=(0.3, 0.1, 0.35), extent=2.4))
    o.polyline(square_xy((-0.2, 0), PROF_H, 0.003), WHITE, width=2.0, closed=True)
    o.line(vtx, np.array([-0.2 + PROF_H, 0.0, 0.6]), HIDDEN, width=1.2)
    o.dot(vtx, 4.5, LINE, outline=(30, 32, 34))
    return o.finish(SIZE)


def end_blind(k, n):
    return _end("blind")


def end_symmetric(k, n):
    return _end("symmetric")


def end_two_sided(k, n):
    return _end("two_sided")


def end_through_all(k, n):
    return _end("through_all")


def end_up_to_face(k, n):
    return _end("up_to_face")


def end_up_to_vertex(k, n):
    return _end("up_to_vertex")


def taper(k, n):
    t = math.tan(math.radians(14.0))

    def scene(p):
        half = 0.45 - p[:, 2] * t
        d2 = np.maximum(np.abs(p[:, 0]), np.abs(p[:, 1])) - half
        return union(bed(p), (np.maximum(d2 * 0.9, np.abs(p[:, 2] - 0.45) - 0.45), M_ACCENT))
    o = shot(scene, cam(SIZE, target=(0.0, 0.0, 0.4), extent=2.2))
    o.polyline(square_xy((0, 0), 0.45, 0.003), WHITE, width=2.0, closed=True)
    return o.finish(SIZE)


# --- which edges a fillet takes when none is picked --------------------------------------------------


def _edges(kind):
    lo, hi = np.array([-0.6, -0.42, 0.0]), np.array([0.6, 0.42, 0.7])
    c = cam(SIZE, target=(0.0, 0.0, 0.35), extent=2.2)
    o = shot(lambda p: union(bed(p), (sd_box(p, 0.5 * (lo + hi), 0.5 * (hi - lo), 0.0), M_PART)), c)
    for a, b in frame(lo, hi):
        vertical = abs(a[2] - b[2]) > 1e-6
        on_top = (not vertical) and a[2] > 0.5
        on_bottom = (not vertical) and a[2] < 0.1
        pick = (kind == "all") or (kind == "top" and on_top) or (kind == "bottom" and on_bottom) or \
               (kind == "lateral" and vertical)
        if pick:
            o.line(a, b, LINE, width=3.4)
    return o.finish(SIZE)


def edges_top(k, n):
    return _edges("top")


def edges_bottom(k, n):
    return _edges("bottom")


def edges_lateral(k, n):
    return _edges("lateral")


def edges_all(k, n):
    return _edges("all")


# --- loft sections -------------------------------------------------------------------------------


def _loft3(ruled):
    radii, zs = (0.55, 0.25, 0.5), (0.0, 0.55, 1.1)

    def r_at(z):
        z = np.clip(z, 0.0, 1.1)
        if ruled:
            return np.interp(z, zs, radii)
        t = z / 1.1  # a smooth curve through the three sections
        return radii[0] * (1 - t) ** 2 + 2 * (2 * radii[1] - 0.5 * (radii[0] + radii[2])) * t * (1 - t) + radii[2] * t ** 2

    def scene(p):
        d = np.maximum((np.hypot(p[:, 0], p[:, 1]) - r_at(p[:, 2])) * 0.8, np.abs(p[:, 2] - 0.55) - 0.55)
        return union(bed(p), (d, M_ACCENT))
    o = shot(scene, cam(SIZE, target=(0.0, 0.0, 0.55), extent=2.2))
    for r, z in zip(radii, zs):
        o.polyline(ring_pts((0, 0), r, z + 0.003), WHITE, width=1.8)
    return o.finish(SIZE)


def loft_smooth(k, n):
    return _loft3(False)


def loft_ruled(k, n):
    return _loft3(True)


# --- datum planes and axes -----------------------------------------------------------------------


def _plane_kind(kind):
    top = 2 * BOX_H[2]
    if kind == "tangent":
        scene = lambda p: union(bed(p), (sd_cylinder(p, (0, 0), 0.45, 0.0, 0.9, 0.02), M_PART))
    else:
        scene = lambda p: union(bed(p), (_box(p), M_PART))
    c = cam(SIZE, target=(0.0, 0.0, 0.4), extent=2.5)
    o = shot(scene, c)
    if kind == "offset":
        quad(o, square_xy((0, 0), 0.85, top + 0.4))
        o.arrow(np.array([0.0, 0.0, top]), np.array([0.0, 0.0, top + 0.38]), WHITE, 1.6, 7.0)
    elif kind == "angle":
        # Through the right top edge, leaning in over the box.
        e = np.array([BOX_H[0], 0.0, top])
        u = np.array([-math.cos(math.radians(40)), 0.0, math.sin(math.radians(40))])
        quad(o, [e + np.array([0, -0.7, 0]), e + np.array([0, 0.7, 0]), e + np.array([0, 0.7, 0]) + u * 1.1,
                 e + np.array([0, -0.7, 0]) + u * 1.1])
        o.line(e + np.array([0, -BOX_H[1], 0]), e + np.array([0, BOX_H[1], 0]), WHITE, 2.4)
    elif kind == "midplane":
        quad(o, [np.array([0.0, y, z]) for y, z in ((-0.7, -0.05), (0.7, -0.05), (0.7, 1.05), (-0.7, 1.05))])
        for x in (-BOX_H[0], BOX_H[0]):
            o.polyline([np.array([x, y, z]) for y, z in ((-BOX_H[1], 0), (BOX_H[1], 0), (BOX_H[1], top), (-BOX_H[1], top))],
                       WHITE, width=1.6, closed=True)
    elif kind == "tangent":
        a = math.radians(-60)
        n_ = np.array([math.cos(a), math.sin(a), 0.0])
        t_ = np.array([-n_[1], n_[0], 0.0])
        p0 = n_ * 0.45
        quad(o, [p0 - t_ * 0.6 + np.array([0, 0, -0.05]), p0 + t_ * 0.6 + np.array([0, 0, -0.05]),
                 p0 + t_ * 0.6 + np.array([0, 0, 1.1]), p0 - t_ * 0.6 + np.array([0, 0, 1.1])])
        o.line(p0 + np.array([0, 0, 0.0]), p0 + np.array([0, 0, 0.9]), WHITE, 2.4)
    elif kind == "two_edges":
        a0, a1 = np.array([-BOX_H[0], BOX_H[1], top]), np.array([BOX_H[0], BOX_H[1], top])
        b0, b1 = np.array([-BOX_H[0], -BOX_H[1], 0.0]), np.array([BOX_H[0], -BOX_H[1], 0.0])
        quad(o, [a0, a1, b1, b0])
        o.line(a0, a1, WHITE, 2.6)
        o.line(b0, b1, WHITE, 2.6)
    else:  # coincident: on a face
        quad(o, [np.array([x, -BOX_H[1] - 0.004, z]) for x, z in ((-0.85, -0.05), (0.85, -0.05), (0.85, top + 0.2), (-0.85, top + 0.2))])
    return o.finish(SIZE)


def plane_offset(k, n):
    return _plane_kind("offset")


def plane_angle(k, n):
    return _plane_kind("angle")


def plane_midplane(k, n):
    return _plane_kind("midplane")


def plane_tangent(k, n):
    return _plane_kind("tangent")


def plane_two_edges(k, n):
    return _plane_kind("two_edges")


def plane_coincident(k, n):
    return _plane_kind("coincident")


def _axis_kind(kind):
    top = 2 * BOX_H[2]
    if kind == "cylinder":
        scene = lambda p: union(bed(p), (sd_cylinder(p, (0, 0), 0.45, 0.0, 0.8, 0.02), M_PART))
    else:
        scene = lambda p: union(bed(p), (_box(p), M_PART))
    c = cam(SIZE, target=(0.0, 0.0, 0.45), extent=2.5)
    o = shot(scene, c)
    if kind == "cylinder":
        o.line(np.array([0, 0, -0.1]), np.array([0, 0, 1.35]), LINE, 3.0)
    elif kind == "two_points":
        a, b = np.array([-BOX_H[0], -BOX_H[1], top]), np.array([BOX_H[0], BOX_H[1], top])
        o.line(a + (a - b) * 0.3, b + (b - a) * 0.3, LINE, 3.0)
        o.dot(a, 4.0, WHITE, outline=(30, 32, 34))
        o.dot(b, 4.0, WHITE, outline=(30, 32, 34))
    elif kind == "face_normal":
        o.polyline([np.array([x, y, top]) for x, y in ((-BOX_H[0], -BOX_H[1]), (BOX_H[0], -BOX_H[1]), (BOX_H[0], BOX_H[1]), (-BOX_H[0], BOX_H[1]))],
                   WHITE, 1.6, closed=True)
        o.line(np.array([0, 0, top]), np.array([0, 0, top + 0.75]), LINE, 3.0)
    elif kind == "planes":
        quad(o, [np.array([0.0, y, z]) for y, z in ((-0.75, -0.05), (0.75, -0.05), (0.75, 1.2), (-0.75, 1.2))],
             (120, 126, 130, 70), (170, 176, 178))
        quad(o, [np.array([x, 0.0, z]) for x, z in ((-0.9, -0.05), (0.9, -0.05), (0.9, 1.2), (-0.9, 1.2))],
             (120, 126, 130, 70), (170, 176, 178))
        o.line(np.array([0, 0, -0.05]), np.array([0, 0, 1.3]), LINE, 3.0)
    else:  # along an edge
        a, b = np.array([-BOX_H[0], -BOX_H[1], top]), np.array([BOX_H[0], -BOX_H[1], top])
        o.line(a - (b - a) * 0.4, b + (b - a) * 0.4, LINE, 3.0)
    return o.finish(SIZE)


def axis_two_points(k, n):
    return _axis_kind("two_points")


def axis_face_normal(k, n):
    return _axis_kind("face_normal")


def axis_cylinder(k, n):
    return _axis_kind("cylinder")


def axis_planes(k, n):
    return _axis_kind("planes")


def axis_edge(k, n):
    return _axis_kind("edge")


ANIMS = ["tool_extrude", "tool_revolve", "tool_sweep", "tool_loft", "tool_dressup", "tool_draft", "tool_shell",
         "tool_hole", "tool_pattern", "tool_cut", "tool_new_sketch", "tool_mirror_body", "tool_move_body",
         "tool_delete_face", "tool_thicken", "tool_rib", "tool_place_on_face", "tool_section"]
STILLS = ["tool_thread", "tool_boolean", "tool_plane", "tool_step", "tool_import_mesh", "tool_mate", "tool_helix",
          "tool_axis", "tool_coordsys", "tool_project", "tool_surface", "tool_surface_extrude", "tool_surface_revolve",
          "tool_surface_loft", "tool_surface_offset", "tool_thicken_surface", "tool_flip_section",
          "commit_assembly", "commit_bodies",
          "end_blind", "end_symmetric", "end_two_sided", "end_through_all", "end_up_to_face", "end_up_to_vertex", "taper",
          "result_new", "result_join", "result_cut", "result_intersect", "bool_target", "bool_tool",
          "keep_tool_off", "keep_tool_on", "edges_top", "edges_bottom", "edges_lateral", "edges_all",
          "dressup_fillet", "dressup_chamfer", "hole_blind", "hole_through", "thread_external", "thread_internal",
          "pattern_linear", "pattern_circular", "loft_smooth", "loft_ruled",
          "plane_offset", "plane_angle", "plane_midplane", "plane_tangent", "plane_two_edges", "plane_coincident",
          "axis_two_points", "axis_face_normal", "axis_cylinder", "axis_planes", "axis_edge",
          "mate_fastened", "mate_planar", "mate_revolute", "mate_slider", "mate_cylindrical"]
FRAMES = {**{a: N_ANIM for a in ANIMS}, **{s: 0 for s in STILLS}}
