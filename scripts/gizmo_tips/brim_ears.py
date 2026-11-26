"""The Brim Ears panel's tooltips (GLGizmoBrimEars::on_render_input_window()).

The ears in these pictures are placed the way Auto-generate places them (GLGizmoBrimEars::generate_points()):
the first layer's outline is simplified by Douglas-Peucker with the detection radius as its tolerance, and
every convex corner of what is left that is sharper than the max angle gets one.
"""

import math

import numpy as np

from .render import M_ACCENT, M_PART, SIZE, Camera, Overlay, bed, extrude, render, sd_polygon2, union

OUT = "gizmos/brim_ears"  # resources/images/<OUT>

HEIGHT = 0.22  # of the part
EAR = 0.22  # ear radius, unless a picture is about it
DEFAULT_MAX_ANGLE = 125.0  # the panel's default


# --- placing the ears, as the gizmo does ----------------------------------------------------------


def _seg_dist(p, a, b):
    ab = b - a
    t = 0.0 if ab @ ab < 1e-18 else min(max(((p - a) @ ab) / (ab @ ab), 0.0), 1.0)
    return float(np.linalg.norm(p - (a + t * ab)))


def _douglas_peucker(pts, tol):
    """MultiPoint::_douglas_peucker(): keeps the first and last point and whatever lies further than tol."""
    if len(pts) < 3:
        return list(pts)
    i_max, d_max = 0, 0.0
    for i in range(1, len(pts) - 1):
        d = _seg_dist(pts[i], pts[0], pts[-1])
        if d > d_max:
            i_max, d_max = i, d
    if d_max <= tol:
        return [pts[0], pts[-1]]
    return _douglas_peucker(pts[: i_max + 1], tol)[:-1] + _douglas_peucker(pts[i_max:], tol)


def _simplified(outline, detection):
    """The outline generate_points() looks for corners on (counter-clockwise)."""
    if detection <= 0.0:
        return list(outline)
    pts = _douglas_peucker(list(outline) + [outline[0]], detection)
    return pts[:-1] if len(pts) > 4 else pts


def _ears(outline, max_angle):
    """Convex corners whose inner angle is below max_angle (Polygon::convex_points())."""
    out = []
    n = len(outline)
    for i in range(n):
        a, b, c = outline[i - 1], outline[i], outline[(i + 1) % n]
        v1, v2 = b - a, c - b
        if v1[0] * v2[1] - v1[1] * v2[0] <= 0.0:
            continue
        turn = math.degrees(math.acos(np.clip(v1 @ v2 / (np.linalg.norm(v1) * np.linalg.norm(v2)), -1.0, 1.0)))
        if 180.0 - turn < max_angle:
            out.append(b)
    return out


def _rounded(corners, radii, segments=10):
    """A polygon (counter-clockwise) with each corner rounded by its radius, as a mesh would carry it."""
    pts = []
    n = len(corners)
    for i in range(n):
        a, b, c = corners[i - 1], corners[i], corners[(i + 1) % n]
        r = radii[i]
        if r <= 0.0:
            pts.append(b)
            continue
        u, w = (a - b) / np.linalg.norm(a - b), (c - b) / np.linalg.norm(c - b)
        half = math.acos(np.clip(u @ w, -1.0, 1.0)) / 2.0
        centre = b + (u + w) / np.linalg.norm(u + w) * r / math.sin(half)
        p0, p1 = b + u * r / math.tan(half), b + w * r / math.tan(half)
        a0, a1 = (math.atan2(*(p - centre)[::-1]) for p in (p0, p1))
        sweep = (a1 - a0 + math.pi) % (2.0 * math.pi) - math.pi
        pts += [centre + r * np.array([math.cos(a0 + sweep * t), math.sin(a0 + sweep * t)]) for t in np.linspace(0, 1, segments + 1)]
    return pts


# --- the scene -------------------------------------------------------------------------------------


def _plate(outline, ears, ear_r=EAR, guide=None):
    """The part (outline extruded) on the bed with its ears; `guide`, a closed line drawn on its top."""
    verts = [tuple(p) for p in outline]

    def scene(p):
        items = [bed(p), (extrude(sd_polygon2(p[:, 0], p[:, 1], verts), 0.0, HEIGHT)(p), M_PART)]
        for e in ears:
            disc = np.hypot(p[:, 0] - e[0], p[:, 1] - e[1]) - ear_r
            items.append((extrude(disc, 0.0, 0.035)(p), M_ACCENT))
        return union(*items)

    cam = Camera(SIZE, target=(0.0, 0.1, 0.1), extent=2.75, el=64.0)
    o = Overlay(render(scene, cam), cam)
    if guide is not None:
        o.polyline([(x, y, HEIGHT + 0.002) for x, y in guide], (235, 242, 242, 220), width=1.4, closed=True)
        for x, y in guide:
            o.dot((x, y, HEIGHT + 0.002), 2.2, (235, 242, 242))
    return o.finish(SIZE)


# --- Brim ear radius: the same ears, small and large -------------------------------------------------

_TRIANGLE = [np.array([0.85 * math.cos(math.radians(a)), 0.85 * math.sin(math.radians(a)) + 0.1]) for a in (90.0, 210.0, 330.0)]


def radius_small(k, n):
    return _plate(_TRIANGLE, _TRIANGLE, ear_r=0.13)


def radius_large(k, n):
    return _plate(_TRIANGLE, _TRIANGLE, ear_r=0.32)


# --- Max angle: one outline with corners of every kind, and which of them get an ear --------------
#
# Two right angles at the front, two 135 degree corners at the back right, a concave notch and a 60
# degree spike at the back left.

_MIXED = [np.array(p) for p in ((-0.85, -0.6), (0.85, -0.6), (0.85, 0.1), (0.35, 0.6), (-0.15, 0.6), (-0.85, 1.0))]


def max_angle_low(k, n):
    return _plate(_MIXED, _ears(_MIXED, 90.0))


def max_angle_high(k, n):
    return _plate(_MIXED, _ears(_MIXED, 160.0))


# --- Detection radius: rounded corners, which only count as corners once the outline is simplified --

_ROUNDED = _rounded([np.array(p) for p in ((-0.8, -0.55), (0.8, -0.55), (0.8, 0.75), (-0.8, 0.75))], [0.0, 0.5, 0.5, 0.0])


def detection_zero(k, n):
    return _plate(_ROUNDED, _ears(_simplified(_ROUNDED, 0.0), DEFAULT_MAX_ANGLE))


def detection_large(k, n):
    simple = _simplified(_ROUNDED, 0.25)
    return _plate(_ROUNDED, _ears(simple, DEFAULT_MAX_ANGLE), guide=simple)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "radius_small": 0,
    "radius_large": 0,
    "max_angle_low": 0,
    "max_angle_high": 0,
    "detection_zero": 0,
    "detection_large": 0,
}
