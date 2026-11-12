"""Tooltips of the general dialogs, second set: the STEP, AMF and 3MF import options (how the bodies
load, how fine a STEP is meshed), the texture import (colour count, smoothing) and the roles of an
IDEX/IQEX mode (Copy, Mirror).
"""

import math

import numpy as np

from . import render as R
from .render import (GIF_SIZE, M_ACCENT, M_PART, M_PART2, SIZE, Camera, Overlay, bed, extrude, render,
                     sd_box, sd_cylinder, sd_polygon2, sd_sphere, union)

OUT = "app_ui"  # resources/images/<OUT>

NOZZLE = (205, 210, 210)
NOZZLE_EDGE = (30, 32, 34)

# render() looks its colours up in R.MATERIALS by index. The pictures here are about filament colour,
# so they append colours of their own after the shared ones, which keep their indices.
_BASE = R.MATERIALS[:8].copy()
FIL_B = np.array([0.80, 0.52, 0.24])  # a second filament colour, beside the teal


def _palette(colours):
    """Puts `colours` after the shared materials and returns the index of the first one."""
    R.MATERIALS = np.vstack([_BASE, np.asarray(colours, float)])
    return len(_BASE)


def _nozzle(o, tip, size=1.0):
    """A small hot end over the point being printed, drawn flat like the pointer (as in app_ui)."""
    x, y = o.pt(tip)
    k = 9.0 * R.SS * size
    o.draw.polygon([(x, y), (x - 0.45 * k, y - 0.7 * k), (x + 0.45 * k, y - 0.7 * k)], fill=NOZZLE, outline=NOZZLE_EDGE,
                   width=max(1, R.SS))
    o.draw.rectangle([x - 0.95 * k, y - 1.9 * k, x + 0.95 * k, y - 0.7 * k], fill=NOZZLE, outline=NOZZLE_EDGE,
                     width=max(1, R.SS))


# --- import: separate objects, an assembly, a single object ----------------------------------------
#
# One model in three bodies - a base, a post and a ball - loaded three ways. As parts of one object the
# bodies take three tones, the way an assembly's parts are told apart; as one object they are one grey.

LOAD_CAM = dict(target=(0.0, 0.0, 0.45), extent=2.6)


def _bodies(p, separate):
    """The base, the post and the ball: spread over the bed, or put together."""
    if separate:
        return (sd_box(p, (-0.45, 0.25, 0.09), (0.42, 0.42, 0.09), 0.02),
                sd_cylinder(p, (0.62, 0.50), 0.12, 0.0, 0.56, 0.01),
                sd_sphere(p, (0.30, -0.62, 0.20), 0.20))
    return (sd_box(p, (0.0, 0.0, 0.09), (0.42, 0.42, 0.09), 0.02),
            sd_cylinder(p, (0.0, 0.0), 0.12, 0.18, 0.74, 0.01),
            sd_sphere(p, (0.0, 0.0, 0.92), 0.20))


def load_separate(k, n):
    def scene(p):
        base, post, ball = _bodies(p, True)
        return union(bed(p), (base, M_ACCENT), (post, M_ACCENT), (ball, M_ACCENT))

    cam = Camera(SIZE, **LOAD_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def load_assembly(k, n):
    def scene(p):
        base, post, ball = _bodies(p, False)
        return union(bed(p), (base, M_PART2), (post, M_PART), (ball, M_ACCENT))

    cam = Camera(SIZE, **LOAD_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def load_single(k, n):
    def scene(p):
        base, post, ball = _bodies(p, False)
        return union(bed(p), (np.minimum(np.minimum(base, post), ball), M_PART))

    cam = Camera(SIZE, **LOAD_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


# --- STEP meshing: coarse and fine facets ------------------------------------------------------------


def _fibonacci(count):
    i = np.arange(count) + 0.5
    phi = np.arccos(1.0 - 2.0 * i / count)
    theta = math.pi * (1.0 + 5.0 ** 0.5) * i
    return np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], axis=1)


def _faceted(sides, facets):
    normals = _fibonacci(facets)
    centre = np.array([0.38, -0.30, 0.42])

    def scene(p):
        # A ring with flat sides, as a STEP cylinder comes out once meshed.
        x, y = p[:, 0] + 0.42, p[:, 1] - 0.22
        ang = np.arctan2(y, x)
        sector = 2.0 * math.pi / sides
        a = (ang + 0.5 * sector) % sector - 0.5 * sector
        r = np.hypot(x, y) * np.cos(a)
        ring2 = np.maximum(r - 0.50, 0.30 - r)
        ring = extrude(ring2, 0.0, 0.42)(p)
        # A ball cut by flat facets: the furthest of the facet planes.
        q = p - centre
        ball = np.concatenate([(q[i:i + 16384] @ normals.T).max(axis=1) for i in range(0, len(q), 16384)]) - 0.40
        return union(bed(p), (ring, M_ACCENT), (ball * 0.9, M_ACCENT))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.32), extent=2.4)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def step_mesh_coarse(k, n):
    return _faceted(9, 34)


def step_mesh_fine(k, n):
    return _faceted(64, 320)


# --- texture import: colour count and smoothing --------------------------------------------------------

TEX_CAM = dict(target=(0.0, 0.0, 0.55), extent=1.75)
BALL = (0.0, 0.0, 0.58)
BALL_R = 0.55


def _hue(h):
    """A soft hue wheel, not the full saturated one, so it sits with the rest of the set."""
    h = np.asarray(h) % 1.0
    k = np.stack([(5.0 + h * 6.0) % 6.0, (3.0 + h * 6.0) % 6.0, (1.0 + h * 6.0) % 6.0], axis=-1)
    rgb = 1.0 - np.clip(np.minimum(k, 4.0 - k), 0.0, 1.0)
    return 0.30 + 0.55 * rgb


def _texture_value(p):
    """Where on the colour wheel the texture is at each point: a sweep round the ball and up it."""
    q = p - np.asarray(BALL)
    ang = np.arctan2(q[:, 1], q[:, 0]) / (2.0 * math.pi)
    return (0.55 * ang + 0.35 * q[:, 2] / BALL_R + 0.08 * np.sin(7.0 * q[:, 0])) % 1.0


def _tex_colours(count):
    first = _palette(_hue((np.arange(count) + 0.5) / count))

    def scene(p):
        v = _texture_value(p)
        mat = first + np.minimum((v * count).astype(int), count - 1)
        return union(bed(p), (sd_sphere(p, BALL, BALL_R), mat))

    cam = Camera(SIZE, **TEX_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def tex_colors_few(k, n):
    return _tex_colours(4)


def tex_colors_many(k, n):
    return _tex_colours(14)


def _tex_smooth(smooth):
    first = _palette([FIL_B, _hue(0.62)])
    cell = 0.17  # the size of the mesh triangles the painting follows

    def boundary(ang):
        return 0.30 * np.sin(2.0 * ang + 0.4) + 0.12 * np.cos(5.0 * ang) - 0.05

    def scene(p):
        q = p - np.asarray(BALL)
        ang = np.arctan2(q[:, 1], q[:, 0])
        z = q[:, 2]
        if smooth:
            upper = z > boundary(ang) * BALL_R
        else:
            # Each triangle of a coarse mesh takes one colour whole, so the edge runs in steps.
            u = ang * BALL_R / cell
            w = z / cell
            iu, iw = np.floor(u), np.floor(w)
            lower_tri = (u - iu) + (w - iw) < 1.0
            cu = (iu + np.where(lower_tri, 1.0 / 3.0, 2.0 / 3.0)) * cell / BALL_R
            cw = (iw + np.where(lower_tri, 1.0 / 3.0, 2.0 / 3.0)) * cell
            upper = cw > boundary(cu) * BALL_R
        mat = np.where(upper, first + 1, first)
        return union(bed(p), (sd_sphere(p, BALL, BALL_R), mat))

    cam = Camera(SIZE, **TEX_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def tex_smooth_low(k, n):
    return _tex_smooth(False)


def tex_smooth_high(k, n):
    return _tex_smooth(True)


# --- IDEX/IQEX roles: the second head copies or mirrors the first -------------------------------------
#
# Two print zones side by side. The Primary prints an arrow pointing right in the left zone; the other
# head prints it at the same time in the right zone, the same way round (Copy) or reflected (Mirror).

ARROW = [(-0.30, -0.13), (0.05, -0.13), (0.05, -0.30), (0.32, 0.0), (0.05, 0.30), (0.05, 0.13), (-0.30, 0.13)]
ZONE_X = (-0.55, 0.55)
IMEX_LAYERS = 6
IMEX_LH = 0.04


def _imex(k, n, mirror):
    printing = 2 * IMEX_LAYERS  # two frames a layer, then the result held
    layers = min(k // 2 + 1, IMEX_LAYERS)
    h = layers * IMEX_LH

    def arrow(p, cx, flip):
        x = (p[:, 0] - cx) * (-1.0 if flip else 1.0)
        d2 = sd_polygon2(x, p[:, 1], ARROW)
        ridge = 0.004 * (0.5 + 0.5 * np.cos(2.0 * math.pi * p[:, 2] / IMEX_LH))
        return extrude(d2, 0.0, h)(p) + ridge

    def scene(p):
        # The boundary between the two zones, a thin line on the bed.
        line = extrude(np.abs(p[:, 0]) - 0.008, 0.0, 0.004)(p)
        return union(bed(p), (line, M_PART2), (arrow(p, ZONE_X[0], False), M_PART),
                     (arrow(p, ZONE_X[1], mirror), M_ACCENT))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.2), extent=2.2)
    o = Overlay(render(scene, cam), cam)
    if k < printing:
        # Both heads at the same point of the same path, one of them reflected.
        t = (k * 0.43) % 1.0
        pts = ARROW + [ARROW[0]]
        seg = int(t * (len(pts) - 1))
        f = t * (len(pts) - 1) - seg
        x = pts[seg][0] + f * (pts[seg + 1][0] - pts[seg][0])
        y = pts[seg][1] + f * (pts[seg + 1][1] - pts[seg][1])
        _nozzle(o, (ZONE_X[0] + x, y, h + 0.02))
        _nozzle(o, (ZONE_X[1] + (-x if mirror else x), y, h + 0.02))
    return o.finish(GIF_SIZE)


def imex_copy(k, n):
    return _imex(k, n, mirror=False)


def imex_mirror(k, n):
    return _imex(k, n, mirror=True)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "load_separate": 0,
    "load_assembly": 0,
    "load_single": 0,
    "step_mesh_coarse": 0,
    "step_mesh_fine": 0,
    "tex_colors_few": 0,
    "tex_colors_many": 0,
    "tex_smooth_low": 0,
    "tex_smooth_high": 0,
    "imex_copy": 20,
    "imex_mirror": 20,
}
