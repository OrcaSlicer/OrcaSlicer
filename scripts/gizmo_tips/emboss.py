"""The Text and SVG tools' panels (GLGizmoEmboss, GLGizmoSVG): what their settings do to the shape.

Both panels share most of their settings - the operation, use surface, distance from the surface,
keep up - so both show the same pictures for them, with the toolbar's 'ABC' strokes as the shape.
"""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmos/emboss"  # resources/images/<OUT>

# --- the letters ---------------------------------------------------------------------------------
#
# The toolbar's 'ABC' (letters2 in render.py), one glyph at a time, so a scene can space, thicken,
# slant or place each letter on its own.

ORIGINS = (-0.48, 0.0, 0.50)  # where each letter sits along the line, at scale 1
STROKE = 0.045  # half the width of a stroke


def _glyph(i, x, y, bold=0.0):
    """Letter i of 'ABC' around its own origin, as a 2D distance field: negative inside."""
    if i == 0:  # A
        d = [sd_segment2(x, y, (-0.15, -0.22), (0.0, 0.22)), sd_segment2(x, y, (0.0, 0.22), (0.15, -0.22)),
             sd_segment2(x, y, (-0.08, -0.05), (0.08, -0.05))]
    elif i == 1:  # B
        d = [sd_segment2(x, y, (-0.11, -0.22), (-0.11, 0.22)), sd_segment2(x, y, (-0.11, 0.22), (0.0, 0.22)),
             sd_arc2(x, y, (0.0, 0.11), 0.11, 270.0, 90.0), sd_segment2(x, y, (-0.11, 0.0), (0.02, 0.0)),
             sd_arc2(x, y, (0.02, -0.11), 0.11, 270.0, 90.0), sd_segment2(x, y, (-0.11, -0.22), (0.02, -0.22))]
    else:  # C
        d = [sd_arc2(x, y, (0.03, 0.0), 0.2, 50.0, 310.0)]
    return np.min(np.stack(d), axis=0) - (STROKE + bold)


def letters(x, y, scale=1.0, gap=0.0, bold=0.0, skew=0.0):
    """'ABC' as a 2D distance field, with extra space between the letters, thicker strokes or a slant."""
    x, y = x / scale, y / scale
    x = x - skew * y
    d = [_glyph(i, x - (ORIGINS[i] + (i - 1) * gap), y, bold) for i in range(3)]
    return np.min(np.stack(d), axis=0) * scale


def _slab(d2, h, h0, h1):
    """A 2D distance field extruded between heights h0 and h1 measured along any direction (h per point)."""
    dz = np.abs(h - 0.5 * (h0 + h1)) - 0.5 * (h1 - h0)
    return np.minimum(np.maximum(d2, dz), 0.0) + np.hypot(np.maximum(d2, 0.0), np.maximum(dz, 0.0))


# --- the operation: part, negative volume, modifier --------------------------------------------

TOP = 0.24  # the plate the letters go on
RELIEF = 0.12


def _plate(p, half=(0.95, 0.6)):
    return sd_box(p, (0, 0, TOP / 2), (half[0], half[1], TOP / 2), 0.02)


def _plate_cam(size):
    return Camera(size, target=(0.0, 0.0, 0.2), extent=2.4, el=42.0)


def part(k, n):
    def scene(p):
        raised = extrude(letters(p[:, 0], p[:, 1]), TOP - 0.02, TOP + RELIEF)(p)
        return union(bed(p), (_plate(p), M_PART), (raised, M_ACCENT))

    cam = _plate_cam(SIZE)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def negative(k, n):
    def scene(p):
        plate = _plate(p)
        hole = extrude(letters(p[:, 0], p[:, 1]), TOP - RELIEF, TOP + 0.5)(p)
        return union(bed(p), (np.maximum(plate, -hole), np.where(-hole > plate, M_ACCENT, M_PART)))

    cam = _plate_cam(SIZE)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def modifier(k, n):
    # A modifier is not printed: the plate stays whole, and what changes is the region inside it that the
    # modifier overlaps. So the plate is drawn with that region marked on it, and the modifier itself as
    # a see-through ghost over it - a blend of the scene with and without it.
    def plate_only(p):
        mark = letters(p[:, 0], p[:, 1]) < 0.0
        return union(bed(p), (_plate(p), np.where(mark, M_ACCENT, M_PART)))

    def with_ghost(p):
        ghost = extrude(letters(p[:, 0], p[:, 1]), TOP - 0.14, TOP + RELIEF)(p)
        return union(plate_only(p), (ghost, M_SUPPORT))

    cam = _plate_cam(SIZE)
    a = render(plate_only, cam)
    b = render(with_ghost, cam)
    return Overlay(0.55 * a + 0.45 * b, cam).finish(SIZE)


# --- use surface: flat, or projected onto a curved model ----------------------------------------
#
# A low barrel vault - a lying cylinder sunk into the bed - with its axis running away from the
# camera, so the arch of its top is seen across the picture and the text runs over the arch.

VAULT_R, VAULT_Z = 1.0, -0.45  # radius, and the height of the axis


def _vault_frame(cam):
    a = np.array([cam.c[0], cam.c[1], 0.0])
    a /= np.linalg.norm(a)  # the axis, towards the camera
    return a, np.cross([0.0, 0.0, 1.0], a)  # and across it, to the right in the picture


def _vault(p, a, r):
    s, u = p @ a, p @ r
    d = np.hypot(u, p[:, 2] - VAULT_Z) - VAULT_R
    d = np.maximum(d, np.abs(s) - 0.7)
    return np.maximum(d, -p[:, 2])


def _use_surface(on):
    cam = Camera(SIZE, target=(0.0, 0.0, 0.4), extent=2.3, el=20.0)
    a, r = _vault_frame(cam)
    top = VAULT_Z + VAULT_R

    def scene(p):
        u, s = p @ r, p @ a
        d2 = letters(u, -s, 1.05)  # tops of the letters away from the camera, so they read upright
        if on:
            # Projected straight down onto the surface, and as deep as set from wherever it lands.
            h = np.hypot(u, p[:, 2] - VAULT_Z) - VAULT_R
            text = np.maximum(_slab(d2, h, -0.03, 0.1), VAULT_Z - p[:, 2])
        else:
            # Flat, touching the surface in the middle: the ends hang in the air over the falling arch.
            text = extrude(d2, top - 0.03, top + 0.1)(p)
        return union(bed(p), (_vault(p, a, r), M_PART), (text * 0.8, M_ACCENT))

    return Overlay(render(scene, cam), cam).finish(SIZE)


def use_surface_off(k, n):
    return _use_surface(False)


def use_surface_on(k, n):
    return _use_surface(True)


# --- per glyph: the text as one block, or each letter placed on the surface on its own -----------

UP_R, UP_H = 0.6, 1.25  # an upright cylinder, the text running around its side


def _per_glyph(on):
    scale, zc = 0.75, 0.62
    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.1, el=35.0)
    # Centred well round from the side that faces the camera, so one end of the line runs off towards
    # the silhouette, where it shows whether it stays on the surface.
    phi0 = math.atan2(cam.c[1], cam.c[0]) + math.radians(30.0)

    def frame(phi):
        q = np.array([UP_R * math.cos(phi), UP_R * math.sin(phi), zc])
        return q, np.array([-math.sin(phi), math.cos(phi), 0.0]), np.array([math.cos(phi), math.sin(phi), 0.0])

    def scene(p):
        items = [bed(p), (sd_cylinder(p, (0, 0), UP_R, 0.0, UP_H, 0.03), M_PART)]
        if on:
            for i in range(3):
                q, t, nrm = frame(phi0 + ORIGINS[i] * scale / UP_R)
                x, h = (p - q) @ t, (p - q) @ nrm
                d2 = _glyph(i, x / scale, (p[:, 2] - zc) / scale) * scale
                items.append((_slab(d2, h, -0.03, 0.09), M_ACCENT))
        else:
            q, t, nrm = frame(phi0)
            x, h = (p - q) @ t, (p - q) @ nrm
            d2 = letters(x, p[:, 2] - zc, scale)
            items.append((_slab(d2, h, -0.03, 0.09), M_ACCENT))
        return union(*items)

    return Overlay(render(scene, cam), cam).finish(SIZE)


def per_glyph_off(k, n):
    return _per_glyph(False)


def per_glyph_on(k, n):
    return _per_glyph(True)


# --- distance from the surface ------------------------------------------------------------------


def distance(k, n):
    # Out of the surface and back into it: lifted clear of it, then sunk until only a sliver shows.
    lift = -0.07 + 0.23 * (0.5 - 0.5 * math.cos(2.0 * math.pi * k / n))

    def scene(p):
        text = extrude(letters(p[:, 0], p[:, 1]), TOP + lift - 0.0, TOP + lift + 0.1)(p)
        return union(bed(p), (_plate(p), M_PART), (text, M_ACCENT))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.25), extent=2.4, el=22.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


# --- keep up: upright wherever it goes, or turned with the surface ------------------------------

BALL_R = 0.75


def _keep_up(on):
    cam = Camera(SIZE, target=(0.0, 0.0, 0.8), extent=2.1, el=30.0)
    c = np.array([0.0, 0.0, BALL_R])
    # A spot above the middle of the side that faces the camera: there, upright is straight up in the picture.
    nrm = cam.c + np.array([0.0, 0.0, 0.45])
    nrm /= np.linalg.norm(nrm)
    up = np.array([0.0, 0.0, 1.0]) - nrm[2] * nrm
    up /= np.linalg.norm(up)
    if not on:
        up = rot_axis(nrm, math.radians(-38.0)) @ up
    right = np.cross(up, nrm)
    q = c + BALL_R * nrm

    def scene(p):
        x, y = (p - q) @ right, (p - q) @ up
        h = np.linalg.norm(p - c, axis=1) - BALL_R
        text = np.maximum(_slab(letters(x, y, 0.62), h, -0.03, 0.08), -((p - c) @ nrm))
        return union(bed(p), (sd_sphere(p, c, BALL_R), M_PART), (text * 0.8, M_ACCENT))

    return Overlay(render(scene, cam), cam).finish(SIZE)


def keep_up_on(k, n):
    return _keep_up(True)


def keep_up_off(k, n):
    return _keep_up(False)


# --- the letters' own shape: spacing, line gap, boldness, skew ----------------------------------


def _relief(k, n, field, half=(0.95, 0.6), extent=2.3):
    def scene(p):
        raised = extrude(field(p[:, 0], p[:, 1]), TOP - 0.02, TOP + 0.08)(p)
        return union(bed(p), (_plate(p, half), M_PART), (raised, M_ACCENT))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.2), extent=extent, el=55.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def char_gap(k, n):
    gap = -0.06 + 0.2 * ease(k / n)
    return _relief(k, n, lambda x, y: letters(x, y, 0.85, gap=gap))


def line_gap(k, n):
    half = 0.24 + 0.14 * ease(k / n)  # half the distance between the two lines
    scale = 0.75
    return _relief(k, n, lambda x, y: np.minimum(letters(x, y - half, scale), letters(x, y + half, scale)), (0.95, 0.75), 2.6)


def boldness(k, n):
    bold = -0.02 + 0.05 * ease(k / n)
    return _relief(k, n, lambda x, y: letters(x, y, 1.0, bold=bold))


def skew(k, n):
    s = 0.5 * ease(k / n)
    return _relief(k, n, lambda x, y: letters(x, y, 1.0, skew=s))


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "part": 0,
    "negative": 0,
    "modifier": 0,
    "use_surface_off": 0,
    "use_surface_on": 0,
    "per_glyph_off": 0,
    "per_glyph_on": 0,
    "distance": 16,
    "keep_up_on": 0,
    "keep_up_off": 0,
    "char_gap": 16,
    "line_gap": 16,
    "boldness": 16,
    "skew": 16,
}
