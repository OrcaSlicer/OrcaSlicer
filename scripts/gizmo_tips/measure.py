"""The Measure and Assemble panels' tooltips (GLGizmoMeasure, GLGizmoAssembly): what each kind of
distance measures, and what the assembly actions do to the moving part."""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmos/measure"  # resources/images/<OUT>

WHITE = (240, 244, 244)
LINE = (60, 200, 180)  # ACCENT, lightened enough to read as a line over the grey model
DOT_OUTLINE = (30, 32, 34)


# --- helpers -------------------------------------------------------------------------------------


def _dashed(o, a, b, colour, width=1.4, dash=0.07):
    a, b = np.asarray(a, float), np.asarray(b, float)
    length = np.linalg.norm(b - a)
    count = max(1, int(length / dash))
    for i in range(0, count, 2):
        o.line(a + (b - a) * (i / count), a + (b - a) * (min(i + 1, count) / count), colour, width)


def _dimension(o, a, b, colour=WHITE, width=1.8, head=7.5):
    """A measured length: a line with an arrowhead at both ends."""
    m = 0.5 * (np.asarray(a, float) + np.asarray(b, float))
    o.arrow(m, a, colour, width=width, head=head)
    o.arrow(m, b, colour, width=width, head=head)


def _picked(o, p, radius=3.6):
    o.dot(p, radius, LINE, outline=DOT_OUTLINE)


def _arc(o, centre, u, v, radius, a0, a1, colour, width=1.8, head=None):
    """An arc in the plane spanned by u and v, from angle a0 to a1 (radians); `head` puts an arrowhead at a1."""
    centre, u, v = (np.asarray(x, float) for x in (centre, u, v))
    pts = [centre + radius * (math.cos(t) * u + math.sin(t) * v) for t in np.linspace(a0, a1, 40)]
    o.polyline(pts, colour, width)
    if head:
        o.arrow(pts[-4], pts[-1], colour, width=width, head=head)


def _frustum(p, centre_xy, z_wide, r_wide, z_narrow, r_narrow):
    """A cone with its top cut off, its wide end at z_wide; z_narrow may be above or below it."""
    lo, hi = min(z_wide, z_narrow), max(z_wide, z_narrow)
    t = np.clip((p[:, 2] - z_wide) / (z_narrow - z_wide), 0.0, 1.0)
    r = r_wide + (r_narrow - r_wide) * t
    rho = np.hypot(p[:, 0] - centre_xy[0], p[:, 1] - centre_xy[1])
    slope = (r_wide - r_narrow) / abs(z_narrow - z_wide)
    side = (rho - r) / math.sqrt(1.0 + slope * slope)
    return np.maximum.reduce([side, lo - p[:, 2], p[:, 2] - hi])


# --- Measure: the kinds of distance ----------------------------------------------------------------
#
# A point on top of a tall block and the front top edge of a low one beside it, where the point lies
# past the edge's end: the one case where the two distances differ, which is when both are listed.

_EDGE = (np.array([-0.9, -0.4, 0.4]), np.array([0.1, -0.4, 0.4]))
_POINT = np.array([0.62, -0.4, 1.0])


def _two_blocks(p):
    low = sd_box(p, (-0.4, 0.0, 0.2), (0.5, 0.4, 0.2), 0.012)
    tall = sd_box(p, (0.675, 0.0, 0.5), (0.225, 0.4, 0.5), 0.012)
    return union(bed(p), (np.minimum(low, tall), M_PART))


def _distance(kind):
    cam = Camera(SIZE, target=(0.05, 0.0, 0.5), extent=2.25)
    o = Overlay(render(_two_blocks, cam), cam)
    o.line(_EDGE[0], _EDGE[1], LINE, width=3.2)
    if kind == "perpendicular":
        foot = np.array([_POINT[0], _EDGE[1][1], _EDGE[1][2]])
        _dashed(o, _EDGE[1], foot, LINE, width=1.6)
        # The right angle at the foot.
        s = 0.07
        o.polyline([foot + (0, 0, s), foot + (-s, 0, s), foot + (-s, 0, 0)], WHITE, width=1.2)
        _dimension(o, _POINT, foot)
    else:
        _dimension(o, _POINT, _EDGE[1])
    _picked(o, _POINT)
    return o.finish(SIZE)


def distance_perpendicular(k, n):
    return _distance("perpendicular")


def distance_direct(k, n):
    return _distance("direct")


def angle(k, n):
    # A block with a sloping top; the angle is the one between its sloping front edge and the upright one.
    z_left, z_right, x0, x1, y0 = 1.0, 0.45, -0.7, 0.7, -0.4
    k_slope = (z_left - z_right) / (x1 - x0)

    def scene(p):
        box = sd_box(p, (0.0, 0.0, 0.6), (0.7, 0.4, 0.6), 0.012)
        roof = (p[:, 2] - z_left + (p[:, 0] - x0) * k_slope) / math.sqrt(1.0 + k_slope * k_slope)
        return union(bed(p), (np.maximum(box, roof), M_PART))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=2.4)
    o = Overlay(render(scene, cam), cam)
    corner = np.array([x1, y0, z_right])
    top_end = np.array([x0, y0, z_left])
    o.line(corner, top_end, LINE, width=3.2)
    o.line(corner, (x1, y0, 0.0), LINE, width=3.2)
    d1 = (top_end - corner) / np.linalg.norm(top_end - corner)
    a1 = math.atan2(d1[2], d1[0])  # in the front face, x to the right and z up
    # From the sloping edge round to the upright one, which points straight down: the angle inside the face.
    _arc(o, corner, (1.0, 0.0, 0.0), (0.0, 0.0, 1.0), 0.28, a1, 1.5 * math.pi, WHITE, width=1.8)
    return o.finish(SIZE)


def _xyz_legs(o, p1, p2, width=2.4):
    """The way from p1 to p2 taken one axis at a time: up first, then across, so the legs stay in the air."""
    a = np.asarray(p1, float)
    b = np.array([a[0], a[1], p2[2]])
    c = np.array([p2[0], a[1], p2[2]])
    _dashed(o, p1, p2, WHITE, width=1.2)
    o.arrow(a, b, AXIS_Z, width=width, head=8.0)
    o.arrow(b, c, AXIS_X, width=width, head=8.0)
    o.arrow(c, p2, AXIS_Y, width=width, head=8.0)


def distance_xyz(k, n):
    def scene(p):
        a = sd_box(p, (-0.55, -0.25, 0.2), (0.3, 0.3, 0.2), 0.012)
        b = sd_box(p, (0.45, 0.35, 0.5), (0.3, 0.3, 0.5), 0.012)
        return union(bed(p), (np.minimum(a, b), M_PART))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.55), extent=2.5)
    o = Overlay(render(scene, cam), cam)
    p1, p2 = np.array([-0.4, -0.4, 0.4]), np.array([0.4, 0.3, 1.0])
    _xyz_legs(o, p1, p2)
    _picked(o, p1)
    _picked(o, p2)
    return o.finish(SIZE)


# --- Assemble ------------------------------------------------------------------------------------
#
# The fixed part is a wide block; the moving part a smaller, darker one. The two picked faces are the
# accent colour: Face 1 the block's top, Face 2 a face of the moving part that starts out tilted.

_BASE_C, _BASE_H = np.array([-0.4, 0.0, 0.25]), np.array([0.55, 0.42, 0.25])
_PART_H = np.array([0.32, 0.26, 0.18])
_R0 = rot_z(math.radians(30.0)) @ rot_x(math.radians(-30.0)) @ rot_y(math.radians(25.0))
_C0 = np.array([0.8, -0.2, 1.2])


def _assembly_camera(size):
    return Camera(size, target=(0.1, 0.0, 0.65), extent=2.8)


def _face2(cam):
    """Face 2: the face of the moving part the camera sees best, so its colour shows from the start."""
    faces = [s * e for e in np.eye(3) for s in (1.0, -1.0)]
    return max(faces, key=lambda f: (_R0 @ f) @ cam.c + 0.6 * (_R0 @ f) @ LIGHT)


def _turn_to_face_down(face):
    """Axis and angle that turn Face 2 from its starting direction to facing straight down."""
    m = _R0 @ face
    axis = np.cross(m, [0.0, 0.0, -1.0])
    return axis, math.acos(np.clip(m @ np.array([0.0, 0.0, -1.0]), -1.0, 1.0))


def _assembly_frame(size, R, c, face, centres=False):
    along = np.abs(face) @ _PART_H
    top = 2.0 * _BASE_H[2]

    def scene(p):
        a = sd_box(p, _BASE_C, _BASE_H, 0.015)
        q = (p - c) @ R
        b = sd_box(q, (0, 0, 0), _PART_H, 0.015)
        return union(bed(p), (a, np.where(p[:, 2] > top - 0.01, M_ACCENT, M_PART)),
                     (b, np.where(q @ face > along - 0.01, M_ACCENT, M_PART2)))

    cam = _assembly_camera(size)
    o = Overlay(render(scene, cam), cam)
    if centres:
        f1 = np.array([_BASE_C[0], _BASE_C[1], top])
        f2 = c + R @ (face * along)
        _dashed(o, f2, f1, WHITE, width=1.2)
        o.dot(f1, 3.0, WHITE, outline=DOT_OUTLINE)
        o.dot(f2, 3.0, WHITE, outline=DOT_OUTLINE)
    return o


def center_coincidence(k, n):
    face = _face2(_assembly_camera(GIF_SIZE))
    axis, angle_ = _turn_to_face_down(face)
    s = smooth(phase(k, 4, 11))
    R = rot_axis(axis, angle_ * s) @ _R0
    along = np.abs(face) @ _PART_H
    c1 = np.array([_BASE_C[0], _BASE_C[1], 2.0 * _BASE_H[2] + along])
    c = _C0 + (c1 - _C0) * s
    return _assembly_frame(GIF_SIZE, R, c, face, centres=s < 0.98).finish(GIF_SIZE)


def parallel(k, n):
    face = _face2(_assembly_camera(GIF_SIZE))
    axis, angle_ = _turn_to_face_down(face)
    s = smooth(phase(k, 4, 9))
    R = rot_axis(axis, angle_ * s) @ _R0
    return _assembly_frame(GIF_SIZE, R, _C0, face).finish(GIF_SIZE)


def _mode_scene(p, circles):
    a = sd_box(p, _BASE_C, _BASE_H, 0.015)
    if not circles:
        return union(bed(p), (a, np.where(p[:, 2] > 2.0 * _BASE_H[2] - 0.01, M_ACCENT, M_PART)))
    peg = sd_cylinder(p, (0.62, 0.25), 0.2, 0.0, 1.05, 0.015)
    return union(bed(p), (a, M_PART), (peg, M_PART2))


# The modes, picked one after the other, are drawn as loops in gizmos_anim.py.


# Flip by Face 2: a narrowing part stands on a table by its wide face. Flipped, the face stays on the
# table top, pointing up now, and the part hangs below it. A table and not a block, so that where it
# goes can be seen.

_TABLE_TOP = 0.82


def _flip(flipped):
    narrow = _TABLE_TOP - 0.55 if flipped else _TABLE_TOP + 0.55
    wide = _TABLE_TOP + (0.004 if flipped else 0.0)

    def scene(p):
        slab = sd_box(p, (0.0, 0.0, _TABLE_TOP - 0.06), (0.9, 0.62, 0.06), 0.012)
        legs = [sd_cylinder(p, (sx * 0.78, sy * 0.5), 0.06, 0.0, _TABLE_TOP - 0.1) for sx in (-1, 1) for sy in (-1, 1)]
        part = _frustum(p, (0.0, -0.05), wide, 0.36, narrow, 0.14)
        if flipped:
            # The wide face, still on the table top: drawn as a thin disc so it is not lost in the slab.
            part = np.minimum(part, sd_cylinder(p, (0.0, -0.05), 0.36, _TABLE_TOP - 0.01, _TABLE_TOP + 0.006))
        return union(bed(p), (np.minimum(slab, np.minimum.reduce(legs)), M_PART), (part, M_ACCENT))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.7), extent=2.5, el=9.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def flip_off(k, n):
    return _flip(False)


def rotate_around_center(k, n):
    a = math.radians(90.0) * ease(k / n)
    top = 2.0 * _BASE_H[2]
    centre = np.array([_BASE_C[0], _BASE_C[1], top])
    R = rot_z(a)

    def scene(p):
        base = sd_box(p, _BASE_C, _BASE_H, 0.015)
        q = (p - centre) @ R
        # An L, so that the turn shows: a long bar and a short arm at one end.
        bar = sd_box(q, (0.0, 0.0, 0.14), (0.34, 0.1, 0.14), 0.012)
        arm = sd_box(q, (0.26, 0.17, 0.14), (0.08, 0.2, 0.14), 0.012)
        return union(bed(p), (base, M_PART), (np.minimum(bar, arm), M_ACCENT))

    cam = Camera(GIF_SIZE, target=(-0.1, 0.0, 0.55), extent=2.3)
    o = Overlay(render(scene, cam), cam)
    _dashed(o, centre, centre + (0.0, 0.0, 0.75), WHITE, width=1.2)
    _arc(o, centre + (0.0, 0.0, 0.62), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.22, math.radians(-60.0),
         math.radians(200.0), WHITE, width=1.8, head=7.0)
    return o.finish(GIF_SIZE)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "distance_perpendicular": 0,
    "distance_direct": 0,
    "angle": 0,
    "distance_xyz": 0,
    "center_coincidence": 20,
    "parallel": 18,
    "flip_off": 0,
    "rotate_around_center": 16,
}
