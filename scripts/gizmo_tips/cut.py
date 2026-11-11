"""The Cut tool panel's tooltips (GLGizmoCut3D): cut modes, connectors, dovetail settings, and what
happens to each part after the cut."""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmos/cut"  # resources/images/<OUT>

PLANE_FILL = (26, 153, 138, 80)
PLANE_EDGE = (60, 200, 180, 220)


def _cut_mat(body, cut):
    """A part bounded by the object `body` and a cut field `cut` (both negative inside): accent where the
    surface is the cut."""
    return np.maximum(body, cut), np.where(cut > body, M_ACCENT, M_PART)


# --- planar and dovetail cuts --------------------------------------------------------------------

BLOCK_C, BLOCK_H = np.array([0.0, 0.0, 0.5]), np.array([0.75, 0.5, 0.5])
ZC = 0.5


def _dovetail(p, depth=0.3, width=0.44, flap=60.0, taper=0.0, with_key=False):
    """The upper part's side of a dovetail cut at ZC, negative inside: everything above the slot, plus the
    key that hangs from it into the lower part. The key runs along y; `taper` narrows it towards -y.
    `with_key` also returns where the key's own faces are rather than the flat cut around it."""
    z = p[:, 2] - ZC
    a = math.radians(flap)
    above = 0.5 * depth - z
    half = 0.5 * width + p[:, 1] * math.tan(math.radians(taper))
    side = (np.abs(p[:, 0]) - half) * math.sin(a) - (0.5 * depth - z) * math.cos(a)
    # The key runs on up into the part rather than stopping at its underside: two fields that both reach
    # zero along a shared face would leave a seam there that the rays stop at.
    key = np.maximum(-z - 0.5 * depth, side)
    return (np.minimum(above, key), key < above) if with_key else np.minimum(above, key)


def _exploded(upper_field, lift):
    """The block cut by `upper_field` (negative on the upper part's side), the upper part lifted by `lift`."""
    def scene(p):
        box = sd_box(p, BLOCK_C, BLOCK_H, 0.02)
        lower = _cut_mat(box, -upper_field(p))
        q = p - np.array([0.0, 0.0, lift])
        up = _cut_mat(sd_box(q, BLOCK_C, BLOCK_H, 0.02), upper_field(q))
        return union(bed(p), lower, up)
    return scene


def _key_mat(part, on_key):
    """Keeps the accent for the key and its slot only: what the dovetail's settings change."""
    return part[0], np.where(on_key, part[1], M_PART)


def _exploded_key(**dovetail):
    """The dovetail cut, exploded, with only the key and its slot in the accent."""
    lift = 0.45

    def scene(p):
        u, on_key = _dovetail(p, with_key=True, **dovetail)
        lower = _key_mat(_cut_mat(sd_box(p, BLOCK_C, BLOCK_H, 0.02), -u), on_key)
        q = p - np.array([0.0, 0.0, lift])
        u, on_key = _dovetail(q, with_key=True, **dovetail)
        up = _key_mat(_cut_mat(sd_box(q, BLOCK_C, BLOCK_H, 0.02), u), on_key)
        return union(bed(p), lower, up)
    return scene


def mode_planar(k, n):
    scene = _exploded(lambda p: ZC - p[:, 2], 0.55)
    cam = Camera(SIZE, target=(0.0, 0.0, 0.75), extent=2.6, az=28.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def mode_dovetail(k, n):
    scene = _exploded(lambda p: _dovetail(p), 0.55)
    cam = Camera(SIZE, target=(0.0, 0.0, 0.75), extent=2.6, az=28.0)
    return Overlay(render(scene, cam, step=0.8), cam).finish(SIZE)


def groove_flap_angle(k, n):
    # From a locking dovetail to a straight tongue and on to one that narrows, and back.
    flap = 45.0 + 70.0 * ease(k / n)
    scene = _exploded_key(depth=0.26, width=0.36, flap=flap)
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.7), extent=2.45, az=22.0, el=20.0)
    return Overlay(render(scene, cam, step=0.8), cam).finish(GIF_SIZE)


def groove_angle(k, n):
    # The lower part alone, from high up, so the slot's outline on its top face shows the taper.
    taper = 14.0 * ease(k / n)

    def scene(p):
        u, on_key = _dovetail(p, depth=0.24, width=0.5, flap=60.0, taper=taper, with_key=True)
        return union(bed(p), _key_mat(_cut_mat(sd_box(p, BLOCK_C, BLOCK_H, 0.02), -u), on_key))

    cam = Camera(GIF_SIZE, target=(0.0, 0.05, 0.5), extent=2.2, az=25.0, el=58.0)
    return Overlay(render(scene, cam, step=0.75), cam).finish(GIF_SIZE)


# --- what happens to a part after the cut -----------------------------------------------------------
#
# A rocket - a foot, a body and a nose cone - so that which way up a part ends up is plain, cut by a
# plane tilted about y, so that Place on cut (which undoes the tilt) differs from Flip (which does not).

TILT = math.radians(28.0)
R_BODY, R_FOOT, Z_FOOT, Z_TOP, Z_TIP, Z_CUT = 0.3, 0.46, 0.12, 1.15, 1.6, 0.72
CUT_N = np.array([math.sin(TILT), 0.0, math.cos(TILT)])  # points into the upper part
CUT_P = np.array([0.0, 0.0, Z_CUT])


def _rocket(p):
    foot = sd_cylinder(p, (0, 0), R_FOOT, 0.0, Z_FOOT, 0.03)
    body = sd_cylinder(p, (0, 0), R_BODY, 0.0, Z_TOP, 0.02)
    # The nose: a cone from the body's top to the tip, as the intersection of a slanted side and its base.
    rho = np.hypot(p[:, 0], p[:, 1])
    s = R_BODY / (Z_TIP - Z_TOP)
    cone = np.maximum((rho - s * (Z_TIP - p[:, 2])) / math.sqrt(1.0 + s * s), Z_TOP - 0.01 - p[:, 2])
    return np.minimum.reduce([foot, body, cone])


def _rocket_points():
    """Points of the rocket's outline: what a part can rest on once turned."""
    a = np.linspace(0.0, 2.0 * math.pi, 96, endpoint=False)
    ring = lambda r, z: np.stack([r * np.cos(a), r * np.sin(a), np.full_like(a, z)], axis=1)
    pts = [ring(R_FOOT, 0.0), ring(R_FOOT, Z_FOOT), ring(R_BODY, Z_FOOT), ring(R_BODY, Z_TOP), [[0.0, 0.0, Z_TIP]]]
    # Where the plane meets the body.
    cut = ring(R_BODY, 0.0)
    cut[:, 2] = Z_CUT - cut[:, 0] * math.tan(TILT)
    return np.concatenate(pts + [cut]), cut


def _part_pose(upper, how):
    """Rotation of the part after the cut, as Cut does it (CutUtils.cpp, reset_instance_transformation)."""
    flip = rot_x(math.pi)
    undo_tilt = rot_y(-TILT)  # the cut matrix's rotation inverted: the cut normal goes to +z
    if how == "on_cut":
        return undo_tilt if upper else flip @ undo_tilt
    if how == "flip":
        return flip
    return np.eye(3)


def _part(upper, how):
    R = _part_pose(upper, how)
    pts, rim = _rocket_points()
    side = (pts - CUT_P) @ CUT_N
    keep = pts[side >= -1e-9] if upper else pts[side <= 1e-9]
    keep = np.concatenate([keep, rim])
    placed = keep @ R.T
    centre = 0.5 * (placed.min(axis=0) + placed.max(axis=0))
    offset = np.array([-centre[0], -centre[1], -placed[:, 2].min()])

    def scene(p):
        q = (p - offset) @ R  # back into the rocket's own frame
        plane = (q - CUT_P) @ CUT_N
        return union(bed(p), _cut_mat(_rocket(q), -plane if upper else plane))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=1.8)
    return Overlay(render(scene, cam, step=0.8), cam).finish(SIZE)


def upper_as_is(k, n):
    return _part(True, "as_is")


def upper_on_cut(k, n):
    return _part(True, "on_cut")


def upper_flip(k, n):
    return _part(True, "flip")


def lower_as_is(k, n):
    return _part(False, "as_is")


def lower_on_cut(k, n):
    return _part(False, "on_cut")


def lower_flip(k, n):
    return _part(False, "flip")


# --- connectors ----------------------------------------------------------------------------------
#
# The two halves laid side by side with their cut faces up, the way they are printed: what each one
# carries shows at once. The connector is the accent; a hole is drawn with accent walls.

HALF_H = np.array([0.42, 0.42, 0.21])
LEFT_C, RIGHT_C = np.array([-0.5, 0.0, 0.21]), np.array([0.5, 0.0, 0.21])
TOP = 0.42


def _hole(p, centre, r, depth):
    return sd_cylinder(p, centre[:2], r, TOP - depth, TOP + 0.5)


def _half_with_hole(p, centre, r, depth):
    box = sd_box(p, centre, HALF_H, 0.02)
    hole = _hole(p, centre, r, depth)
    return np.maximum(box, -hole), np.where(-hole > box, M_ACCENT, M_PART)


def _snap_peg(p, centre, r, h, bulge, space, z0):
    """The snap connector (TriangleMesh.cpp, its_make_snap): a peg swelling to (1 + bulge) r halfway up and
    narrowing to r / 2 at its tip, split by a slit 2 * space * r wide."""
    rho = np.hypot(p[:, 0] - centre[0], p[:, 1] - centre[1])
    t = (p[:, 2] - z0) / h
    mid = (1.0 + bulge) * r
    prof = np.where(t < 0.5, r + (mid - r) * (t / 0.5), mid + (0.5 * r - mid) * ((t - 0.5) / 0.5))
    side = (rho - prof) * 0.85
    solid = np.maximum(side, np.maximum(z0 - p[:, 2], p[:, 2] - (z0 + h)))
    slit = space * r - np.abs(p[:, 0] - centre[0])
    return np.maximum(solid, slit)


def _connectors_cam():
    return Camera(SIZE, target=(0.0, -0.05, 0.3), extent=2.2, el=36.0)


def connector_plug(k, n):
    r, h = 0.15, 0.3

    def scene(p):
        peg = sd_cylinder(p, LEFT_C[:2], r, TOP - 0.02, TOP + h, 0.015)
        return union(bed(p), (sd_box(p, LEFT_C, HALF_H, 0.02), M_PART), (peg, M_ACCENT),
                     _half_with_hole(p, RIGHT_C, r, h))

    cam = _connectors_cam()
    return Overlay(render(scene, cam), cam).finish(SIZE)


def connector_dowel(k, n):
    r, h = 0.14, 0.26
    pin_c = np.array([0.0, -0.78, r])

    def scene(p):
        # The pin lies on the bed in front of the halves, along x.
        q = (p - pin_c) @ rot_y(math.pi / 2.0)
        pin = sd_cylinder(q, (0, 0), r, -h, h, 0.02)
        return union(bed(p), _half_with_hole(p, LEFT_C, r, h), _half_with_hole(p, RIGHT_C, r, h), (pin, M_ACCENT))

    cam = Camera(SIZE, target=(0.0, -0.25, 0.25), extent=2.3, el=36.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def connector_snap(k, n):
    r, h = 0.14, 0.44

    def scene(p):
        peg = _snap_peg(p, LEFT_C, r, h, bulge=0.25, space=0.25, z0=TOP - 0.02)
        return union(bed(p), (sd_box(p, LEFT_C, HALF_H, 0.02), M_PART), (peg, M_ACCENT),
                     _half_with_hole(p, RIGHT_C, r, h))

    cam = _connectors_cam()
    return Overlay(render(scene, cam, step=0.8), cam).finish(SIZE)


def _style(frustum):
    centre, half = np.array([0.0, 0.0, 0.2]), np.array([0.62, 0.48, 0.2])
    r, h, z0 = 0.26, 0.5, 0.4

    def scene(p):
        if frustum:
            # Narrowing to half its radius at the tip, as its_make_frustum() makes it.
            rho = np.hypot(p[:, 0], p[:, 1])
            s = 0.5 * r / h
            side = (rho - (r - s * (p[:, 2] - z0))) / math.sqrt(1.0 + s * s)
            peg = np.maximum(side, np.maximum(z0 - 0.02 - p[:, 2], p[:, 2] - (z0 + h)))
        else:
            peg = sd_cylinder(p, (0, 0), r, z0 - 0.02, z0 + h, 0.015)
        return union(bed(p), (sd_box(p, centre, half, 0.02), M_PART), (peg, M_ACCENT))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.42), extent=1.9)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def style_prism(k, n):
    return _style(False)


def style_frustum(k, n):
    return _style(True)


# --- flipping the cut plane: the plug changes sides ---------------------------------------------------

def _flip_plane(flipped):
    c_lo, c_up, half = np.array([0.0, 0.0, 0.24]), np.array([0.0, 0.0, 1.34]), np.array([0.62, 0.45, 0.24])
    r, h = 0.14, 0.28
    z_lo, z_up = 0.48, 1.1  # the lower part's top, the upper part's underside
    z_plane = 0.5 * (z_lo + z_up)

    def scene(p):
        lo = sd_box(p, c_lo, half, 0.02)
        up = sd_box(p, c_up, half, 0.02)
        if not flipped:
            peg = sd_cylinder(p, (0, 0), r, z_lo - 0.02, z_lo + h, 0.015)
            hole = sd_cylinder(p, (0, 0), r, z_up - 0.5, z_up + h)
            return union(bed(p), (lo, M_PART), (peg, M_ACCENT), (np.maximum(up, -hole), M_PART))
        peg = sd_cylinder(p, (0, 0), r, z_up - h, z_up + 0.02, 0.015)
        hole = sd_cylinder(p, (0, 0), r, z_lo - h, z_lo + 0.5)
        return union(bed(p), (np.maximum(lo, -hole), np.where(-hole > lo, M_ACCENT, M_PART)), (up, M_PART),
                     (peg, M_ACCENT))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.82), extent=2.45, el=24.0)
    o = Overlay(render(scene, cam), cam)
    # The cut plane between the parts, and its normal: the side it points to is the one with the holes.
    sq = [np.array([x, y, z_plane]) for x, y in ((-0.85, -0.65), (0.85, -0.65), (0.85, 0.65), (-0.85, 0.65))]
    o.draw.polygon([o.pt(q) for q in sq], fill=PLANE_FILL)
    o.polyline(sq, PLANE_EDGE, width=1.4, closed=True)
    tail = np.array([0.62, -0.42, z_plane])
    o.arrow(tail, tail + np.array([0.0, 0.0, -0.3 if flipped else 0.3]), (235, 242, 242), width=2.2, head=8.0)
    return o.finish(SIZE)


def flip_plane_a(k, n):
    return _flip_plane(False)


def flip_plane_b(k, n):
    return _flip_plane(True)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "mode_planar": 0,
    "mode_dovetail": 0,
    "groove_flap_angle": 18,
    "groove_angle": 16,
    "upper_as_is": 0,
    "upper_on_cut": 0,
    "upper_flip": 0,
    "lower_as_is": 0,
    "lower_on_cut": 0,
    "lower_flip": 0,
    "connector_plug": 0,
    "connector_dowel": 0,
    "connector_snap": 0,
    "style_prism": 0,
    "style_frustum": 0,
    "flip_plane_a": 0,
    "flip_plane_b": 0,
}
