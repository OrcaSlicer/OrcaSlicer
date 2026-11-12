"""The painting panels' tooltips: Support painting, Seam painting, Paint-on fuzzy skin and Color painting
(GLGizmoFdmSupports, GLGizmoSeam, GLGizmoFuzzySkin, GLGizmoMmuSegmentation). They share one tool row,
so a tool is shown by the same picture in each of them.

ACCENT is paint, as everywhere in the set. PART2 is paint of another filament in the color painting
pictures, and OVERHANG is the overhang highlight of support painting, in the tint the 3D view gives it.
"""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmos/painting"  # resources/images/<OUT>

PATH = (235, 242, 242, 190)  # the way the mouse went, drawn over the model
WIRE = (215, 222, 222, 95)  # the mesh's triangles


# --- helpers -------------------------------------------------------------------------------------


def _shade(d, p, fn, default=M_PART):
    """Materials for a body: `fn` (points -> materials) is asked only about the points near its surface,
    since those are the only ones whose material is ever looked at, and the masks here are not cheap."""
    m = np.full(len(d), default)
    near = np.abs(d) < 0.02
    if near.any():
        m[near] = fn(p[near])
    return m


def _normals(sdf, p, e=2e-3):
    n = np.stack([sdf(p + o) - sdf(p - o) for o in np.eye(3) * e], axis=1)
    return n / (np.linalg.norm(n, axis=1, keepdims=True) + 1e-12)


def _screen(cam, p):
    q = np.atleast_2d(p) - cam.target
    return np.stack([q @ cam.right, q @ cam.up], axis=1)


def _screen_circle(o, centre, radius, colour, width=1.6):
    """A circle as the screen sees it, around a point of the scene: the circle brush's cursor."""
    x, y = o.pt(centre)
    r = radius / o.cam.extent * o.cam.px
    o.draw.ellipse([x - r, y - r, x + r, y + r], outline=colour, width=max(1, int(round(width * SS))))


def _dashed(o, pts, colour, width=1.3, every=2):
    for i in range(0, len(pts) - 1, every):
        o.line(pts[i], pts[min(i + 1, len(pts) - 1)], colour, width)


def _lathe(p, profile, z0, z1, k=0.5):
    """A body of revolution about the z axis, radius profile(z), from z0 to z1. `k` keeps the march safe
    where the profile slopes."""
    rho = np.hypot(p[:, 0], p[:, 1])
    side = (rho - profile(p[:, 2])) * k
    return np.maximum.reduce([side, z0 - p[:, 2], p[:, 2] - z1])


def _seg_dist(p, a, b):
    ab = b - a
    h = np.clip(((p - a) @ ab) / max(ab @ ab, 1e-12), 0.0, 1.0)
    return np.linalg.norm(p - (a + h[:, None] * ab), axis=1)


def _path_dist(p, pts):
    return np.min(np.stack([_seg_dist(p, pts[i], pts[i + 1]) for i in range(len(pts) - 1)]), axis=0)


# The view direction's horizontal part points from the model towards the camera; the side of a round
# model that faces the camera is at this angle.
FRONT = math.atan2(-math.cos(math.radians(35.0)), math.sin(math.radians(35.0)))


# --- Circle and sphere brush ---------------------------------------------------------------------
#
# A block with a trench across it, the brush on the near rim. The circle paints whatever it covers as
# seen from here, down into the trench; the sphere only the surface within its ball.


_TRENCH_R = rot_z(math.radians(14.0))  # the trench runs nearly across the view, so the camera looks into it


def _trench_body(p):
    q = p @ _TRENCH_R
    block = sd_box(q, (0.0, 0.0, 0.45), (0.85, 0.85, 0.45), 0.02)
    cut = sd_box(q, (0.0, 0.12, 0.95), (1.3, 0.36, 0.65), 0.02)
    return np.maximum(block, -cut)


_BRUSH_C = _TRENCH_R @ np.array([-0.05, -0.36, 0.9])
_BRUSH_R = 0.46


def _brush(kind):
    cam = Camera(SIZE, target=(0.0, 0.0, 0.4), extent=2.6, el=40.0)

    def painted(q):
        if kind == "sphere":
            inside = np.linalg.norm(q - _BRUSH_C, axis=1) < _BRUSH_R
        else:
            n = _normals(_trench_body, q)
            inside = (np.linalg.norm(_screen(cam, q) - _screen(cam, _BRUSH_C), axis=1) < _BRUSH_R) & (n @ cam.c > 0.05)
        return np.where(inside, M_ACCENT, M_PART)

    def scene(p):
        d = _trench_body(p)
        return union(bed(p), (d, _shade(d, p, painted)))

    o = Overlay(render(scene, cam), cam)
    _screen_circle(o, _BRUSH_C, _BRUSH_R, CURSOR)
    if kind == "sphere":
        o.circle_on(_BRUSH_C, (0.0, 0.0, 1.0), _BRUSH_R, (235, 242, 242, 150), width=1.2)
    o.pointer(_BRUSH_C)
    return o.finish(SIZE)


def brush_circle(k, n):
    return _brush("circle")


def brush_sphere(k, n):
    return _brush("sphere")


# --- Triangle --------------------------------------------------------------------------------------

_TRI_S = 0.35  # a mesh cell; each is split into two triangles along its diagonal
_TRI_H = np.array([0.7, 0.7, 0.3])
_TRI_CLICKS = [(1, 1, 0), (1, 1, 1), (2, 1, 1), (1, 2, 0)]


def _tri_corners(i, j, t):
    x0, y0, z = -_TRI_H[0] + i * _TRI_S, -_TRI_H[1] + j * _TRI_S, 2.0 * _TRI_H[2]
    a, b, c, d = (x0, y0, z), (x0 + _TRI_S, y0, z), (x0 + _TRI_S, y0 + _TRI_S, z), (x0, y0 + _TRI_S, z)
    return [np.array(v) for v in ((a, b, c) if t == 0 else (a, c, d))]


def tool_triangle(k, n):
    step = 4
    clicked = _TRI_CLICKS[: min(len(_TRI_CLICKS), (k + 1) // step)]
    target = _TRI_CLICKS[min(k // step, len(_TRI_CLICKS) - 1)]

    def painted(q):
        on_top = q[:, 2] > 2.0 * _TRI_H[2] - 0.01
        u = (q[:, 0] + _TRI_H[0]) / _TRI_S
        v = (q[:, 1] + _TRI_H[1]) / _TRI_S
        i, j = np.floor(u).astype(int), np.floor(v).astype(int)
        t = ((v - j) > (u - i)).astype(int)
        hit = np.zeros(len(q), bool)
        for ci, cj, ct in clicked:
            hit |= (i == ci) & (j == cj) & (t == ct)
        return np.where(on_top & hit, M_ACCENT, M_PART)

    def scene(p):
        d = sd_box(p, (0.0, 0.0, _TRI_H[2]), _TRI_H, 0.004)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=2.3, el=42.0)
    o = Overlay(render(scene, cam), cam)
    hx, hy, top = _TRI_H[0], _TRI_H[1], 2.0 * _TRI_H[2]
    for g in np.arange(-hx, hx + 1e-6, _TRI_S):  # the top's grid and diagonals
        o.line((g, -hy, top), (g, hy, top), WIRE, 1.0)
        o.line((-hx, g, top), (hx, g, top), WIRE, 1.0)
    for i in range(4):
        for j in range(4):
            a, _, c = _tri_corners(i, j, 0)
            o.line(a, c, WIRE, 1.0)
    for x in np.arange(-hx, hx + 1e-6, _TRI_S):  # the front side, cells half as tall
        o.line((x, -hy, 0.0), (x, -hy, top), WIRE, 1.0)
        if x < hx - 1e-6:
            o.line((x, -hy, 0.0), (x + _TRI_S, -hy, top), WIRE, 1.0)
    for y in np.arange(-hy, hy + 1e-6, _TRI_S):  # the right side
        o.line((hx, y, 0.0), (hx, y, top), WIRE, 1.0)
        if y < hy - 1e-6:
            o.line((hx, y, 0.0), (hx, y + _TRI_S, top), WIRE, 1.0)
    if k < len(_TRI_CLICKS) * step:
        corners = _tri_corners(*target)
        o.polyline(corners, CURSOR, width=1.8, closed=True)
        o.pointer(sum(corners) / 3.0)
    return o.finish(GIF_SIZE)


# --- Height range ----------------------------------------------------------------------------------


def tool_height_range(k, n):
    r, band = 0.5, 0.3
    z = 0.2 + 0.55 * smooth(phase(k, 0, 10))
    painted_band = k >= 11

    def painted(q):
        inside = painted_band & (q[:, 2] > z - 0.005) & (q[:, 2] < z + band) & (np.hypot(q[:, 0], q[:, 1]) > r - 0.02)
        return np.where(inside, M_ACCENT, M_PART)

    def scene(p):
        d = sd_cylinder(p, (0, 0), r, 0.0, 1.3, 0.03)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.62), extent=2.3)
    o = Overlay(render(scene, cam), cam)
    if not painted_band:
        front = np.linspace(FRONT - math.pi / 2.0, FRONT + math.pi / 2.0, 40)
        for h in (z, z + band):
            o.polyline([np.array([(r + 0.01) * math.cos(a), (r + 0.01) * math.sin(a), h]) for a in front], CURSOR, width=1.6)
    if k < 13:
        o.pointer(np.array([r * math.cos(FRONT + 0.25), r * math.sin(FRONT + 0.25), z]))
    return o.finish(GIF_SIZE)


# --- Smart fill ------------------------------------------------------------------------------------
#
# A round post on a base: the fill runs all the way round the post's side, whose facets meet at shallow
# angles, and stops at its sharp rims.


def tool_smart_fill(k, n):
    r, z0, z1 = 0.42, 0.3, 1.25
    a0, zc = FRONT + 0.3, 0.72
    grow = smooth(phase(k, 5, 7)) * 2.4 if k >= 5 else -1.0

    def painted(q):
        rho = np.hypot(q[:, 0], q[:, 1])
        side = (rho > r - 0.01) & (q[:, 2] > z0 + 0.01) & (q[:, 2] < z1 - 0.01)
        da = np.abs((np.arctan2(q[:, 1], q[:, 0]) - a0 + math.pi) % (2.0 * math.pi) - math.pi) * r
        reach = np.hypot(da, q[:, 2] - zc) < grow
        return np.where(side & reach, M_ACCENT, M_PART)

    def body(p):
        base = sd_box(p, (0.0, 0.0, z0 / 2.0), (0.8, 0.8, z0 / 2.0), 0.02)
        post = sd_cylinder(p, (0, 0), r, z0 - 0.05, z1, 0.0)
        return np.minimum(base, post)

    def scene(p):
        d = body(p)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.6), extent=2.3)
    o = Overlay(render(scene, cam), cam)
    if k < 14:
        o.pointer(np.array([r * math.cos(a0), r * math.sin(a0), zc]))
    return o.finish(GIF_SIZE)


# A block with its top edges chamfered at 45 degrees and a round boss on top. A low angle keeps the fill
# on the top face; a high one carries it over the chamfers and down the sides, but not up the boss,
# whose wall stands at 90 degrees to the top.

_CH_H, _CH_C, _BOSS = 0.4, 0.2, (0.12, 0.18, 0.2)


def _chamfer_body(p):
    box = sd_box(p, (0.0, 0.0, _CH_H), (0.6, 0.6, _CH_H), 0.0)
    lim = 0.6 + 2.0 * _CH_H - _CH_C
    cx = (np.abs(p[:, 0]) + p[:, 2] - lim) / math.sqrt(2.0)
    cy = (np.abs(p[:, 1]) + p[:, 2] - lim) / math.sqrt(2.0)
    boss = sd_cylinder(p, _BOSS[:2], _BOSS[2], 2.0 * _CH_H - 0.05, 2.0 * _CH_H + 0.24, 0.0)
    return np.minimum(np.maximum.reduce([box, cx, cy]), boss)


def _smart_fill_angle(high):
    top = 2.0 * _CH_H

    def painted(q):
        on_boss = (np.hypot(q[:, 0] - _BOSS[0], q[:, 1] - _BOSS[1]) < _BOSS[2] + 0.01) & (q[:, 2] > top - 0.01)
        if high:
            hit = ~on_boss & (q[:, 2] > 0.01)
        else:
            hit = ~on_boss & (q[:, 2] > top - 0.01)
        return np.where(hit, M_ACCENT, M_PART)

    def scene(p):
        d = _chamfer_body(p)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.45), extent=2.3)
    o = Overlay(render(scene, cam), cam)
    o.pointer((-0.22, -0.2, top))
    return o.finish(SIZE)


def smart_fill_angle_low(k, n):
    return _smart_fill_angle(False)


def smart_fill_angle_high(k, n):
    return _smart_fill_angle(True)


# --- Bucket fill and edge detection (color painting) ----------------------------------------------


def tool_bucket_fill(k, n):
    # A rounded post with a band of another filament round its middle: the fill takes the area of the
    # color clicked, up to where the other color begins.
    r, z_mid = 0.45, 0.8
    band = (0.42, 0.58)
    click = np.array([r * math.cos(FRONT + 0.2) * 0.97, r * math.sin(FRONT + 0.2) * 0.97, 0.98])
    grow = smooth(phase(k, 5, 7)) * 1.6 if k >= 5 else -1.0

    def body(p):
        return np.minimum(sd_cylinder(p, (0, 0), r, 0.0, z_mid, 0.02), sd_sphere(p, (0.0, 0.0, z_mid), r))

    def painted(q):
        in_band = (q[:, 2] > band[0]) & (q[:, 2] < band[1])
        filled = (q[:, 2] >= band[1]) & (np.linalg.norm(q - click, axis=1) < grow)
        return np.where(in_band, M_PART2, np.where(filled, M_ACCENT, M_PART))

    def scene(p):
        d = body(p)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.62), extent=2.2)
    o = Overlay(render(scene, cam), cam)
    if k < 14:
        o.pointer(click)
    return o.finish(GIF_SIZE)


def _edge_detection(on):
    h = 0.5
    band = (0.34, 0.5)

    def painted(q):
        in_band = (q[:, 2] > band[0]) & (q[:, 2] < band[1])
        top = q[:, 2] > 2.0 * h - 0.01
        filled = top if on else (q[:, 2] >= band[1])
        return np.where(in_band, M_PART2, np.where(filled, M_ACCENT, M_PART))

    def scene(p):
        d = sd_box(p, (0.0, 0.0, h), (h, h, h), 0.01)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=2.2)
    o = Overlay(render(scene, cam), cam)
    o.pointer((-0.1, -0.15, 2.0 * h))
    return o.finish(SIZE)


def edge_detection_off(k, n):
    return _edge_detection(False)


def edge_detection_on(k, n):
    return _edge_detection(True)


# --- Gap fill --------------------------------------------------------------------------------------
#
# The front of a wall, painted on the left. Before: the specks and slivers a brush leaves behind - holes
# in the paint, and stray dots of it outside. After: each one merged into the paint around it.

_WALL = (0.75, 0.2, 0.6)


def _tri2(cx, cz, s, rot):
    return [(cx + s * math.cos(rot + a), cz + s * math.sin(rot + a)) for a in (0.0, 2.1, 4.2)]


_HOLES = [_tri2(-0.45, 0.85, 0.07, 0.3), _tri2(-0.2, 0.42, 0.06, 1.1), _tri2(-0.5, 0.3, 0.05, 2.0),
          _tri2(-0.15, 0.95, 0.05, 0.7)]
_SPECKS = [_tri2(0.3, 0.75, 0.06, 0.5), _tri2(0.45, 0.35, 0.05, 1.7), _tri2(0.15, 0.22, 0.045, 2.6)]


def _gap_fill(after):
    y = -_WALL[1]

    def edge(z):
        return 0.02 + 0.07 * np.sin(z * 6.0)

    def painted(q):
        x, z = q[:, 0], q[:, 2]
        front = q[:, 1] < y + 0.01
        paint = x < edge(z)
        if not after:
            for tri in _HOLES:
                paint &= sd_polygon2(x, z, tri) > 0.0
            for tri in _SPECKS:
                paint |= sd_polygon2(x, z, tri) < 0.0
        return np.where(front & paint, M_ACCENT, M_PART)

    def scene(p):
        d = sd_box(p, (0.0, 0.0, _WALL[2]), _WALL, 0.02)
        return union(bed(p), (d, _shade(d, p, painted)))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.6), extent=2.2, az=28.0, el=22.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def gap_fill_before(k, n):
    return _gap_fill(False)


def gap_fill_after(k, n):
    return _gap_fill(True)


# Vertical and Horizontal: drawn as loops in gizmos_anim.py.


# --- Support painting: overhang highlight ----------------------------------------------------------
#
# A post that widens upwards in two steps, a steep one (60 degrees from the bed) low down and a shallow
# one (30 degrees) higher up. A low highlight angle marks only the shallow overhang, a high one both.

_CAKE = [(0.0, 0.22), (0.35, 0.22), (0.35 + 0.28 * math.tan(math.radians(60.0)), 0.5), (1.0, 0.5),
         (1.0 + 0.35 * math.tan(math.radians(30.0)), 0.85), (1.4, 0.85)]


def _cake_profile(z):
    zs = [c[0] for c in _CAKE]
    rs = [c[1] for c in _CAKE]
    return np.interp(z, zs, rs)


def _cake(p):
    return _lathe(p, _cake_profile, 0.0, _CAKE[-1][0], k=0.48)


def _cake_scene(threshold_deg, stroke=None):
    limit = -math.cos(math.radians(threshold_deg))

    def material(q):
        n = _normals(_cake, q)
        overhang = n[:, 2] < limit
        m = np.where(overhang, M_OVERHANG, M_PART)
        if stroke is not None:
            painted, only_overhangs = stroke
            hit = painted(q) & (overhang if only_overhangs else True)
            m = np.where(hit, np.where(n[:, 2] < -0.2, M_ACCENT_UNDER, M_ACCENT), m)
        return m

    def scene(p):
        d = _cake(p)
        return union(bed(p), (d, _shade(d, p, material)))

    return scene


def _cake_camera():
    return Camera(SIZE, target=(0.0, 0.0, 0.68), extent=2.35, el=13.0)


def overhang_highlight_low(k, n):
    cam = _cake_camera()
    return Overlay(render(_cake_scene(40.0), cam), cam).finish(SIZE)


def overhang_highlight_high(k, n):
    cam = _cake_camera()
    return Overlay(render(_cake_scene(70.0), cam), cam).finish(SIZE)


def _overhangs_only(on):
    a, width = FRONT + 0.15, 0.13

    def painted(q):
        da = np.abs((np.arctan2(q[:, 1], q[:, 0]) - a + math.pi) % (2.0 * math.pi) - math.pi)
        return (da * np.hypot(q[:, 0], q[:, 1]) < width) & (q[:, 2] > 0.12)

    cam = _cake_camera()
    o = Overlay(render(_cake_scene(70.0, (painted, on)), cam), cam)
    end = np.array([0.22 * math.cos(a), 0.22 * math.sin(a), 0.18])
    o.circle_on(end, (math.cos(a), math.sin(a), 0.0), width, CURSOR)
    o.pointer(end)
    return o.finish(SIZE)


def overhangs_only_off(k, n):
    return _overhangs_only(False)


def overhangs_only_on(k, n):
    return _overhangs_only(True)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "brush_circle": 0,
    "brush_sphere": 0,
    "tool_triangle": 20,
    "tool_height_range": 18,
    "tool_smart_fill": 18,
    "smart_fill_angle_low": 0,
    "smart_fill_angle_high": 0,
    "tool_bucket_fill": 18,
    "edge_detection_off": 0,
    "edge_detection_on": 0,
    "gap_fill_before": 0,
    "gap_fill_after": 0,
    "overhang_highlight_low": 0,
    "overhang_highlight_high": 0,
    "overhangs_only_off": 0,
    "overhangs_only_on": 0,
}
