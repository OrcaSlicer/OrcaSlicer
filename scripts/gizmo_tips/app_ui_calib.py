"""Tooltips of the calibration dialogs (calib_dlg.cpp): the test each dialog prints, with the part an input
sets in the accent colour. Most tests change one value with height, so their start, end and step inputs
share one model drawn in bands, one band per value, and differ in which band is the accent: the bottom
one for the start, the top one for the end, one in the middle for the step.

The shapes follow the models in resources/calib, simplified to what reads at tooltip size.
"""

import math

import numpy as np

from .render import (M_ACCENT, M_PART, M_PART2, SIZE, Camera, Overlay, bed, extrude, render, sd_box,
                     sd_cylinder, sd_segment2, union)

OUT = "app_ui"  # resources/images/<OUT>

TOWER_CAM = dict(target=(0.0, 0.0, 0.62), extent=2.75)
LOW_CAM = dict(target=(0.0, 0.0, 0.35), extent=2.7)
FLAT_CAM = dict(target=(0.0, 0.0, 0.0), extent=2.3)


def _still(scene, cam_args):
    cam = Camera(SIZE, **cam_args)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def _bands(z, band, count, accent):
    """Alternating greys, one band per value up the model, the bands in `accent` in the accent colour."""
    idx = np.clip(np.floor(z / band), 0, count - 1).astype(int)
    mat = np.where(idx % 2 == 0, M_PART, M_PART2)
    return np.where(np.isin(idx, list(accent)), M_ACCENT, mat)


def _which(kind, count):
    """The band an input sets: the first for the start, the last for the end, one in the middle for the step."""
    return {"start": [0], "end": [count - 1], "step": [count // 2]}[kind]


# --- temperature tower: a stack of blocks, one per temperature --------------------------------------

TEMP_BLOCKS = 5
TEMP_BH = 0.25
TEMP_HALF = (0.68, 0.17)


def _temp_block(p, i):
    """One block of the tower: its left end cut away underneath at 45 degrees (the overhang) and a
    bridge across a window, as each block of the real tower has."""
    z0 = i * TEMP_BH
    x, z = p[:, 0], p[:, 2] - z0
    body = sd_box(p, (0.0, 0.0, z0 + TEMP_BH * 0.5), (TEMP_HALF[0], TEMP_HALF[1], TEMP_BH * 0.5 - 0.004), 0.006)
    xw = -0.30  # where the overhang starts
    wedge = (x - (xw - z)) / math.sqrt(2.0)  # negative below the 45 degree line
    body = np.maximum(body, -np.maximum(-wedge, x - xw))
    window = sd_box(p, (0.28, 0.0, z0 + 0.12), (0.17, 0.5, 0.06))
    return np.maximum(body, -window)


def _temp_tower(accent):
    def scene(p):
        items = [bed(p)]
        for i in range(TEMP_BLOCKS):
            mat = M_ACCENT if i in accent else (M_PART if i % 2 == 0 else M_PART2)
            items.append((_temp_block(p, i), mat))
        return union(*items)
    return _still(scene, TOWER_CAM)


def calib_temp_start(k, n):
    return _temp_tower([0])


def calib_temp_end(k, n):
    return _temp_tower([TEMP_BLOCKS - 1])


def calib_temp_standard(k, n):
    """The standard tower: every block is compared, so the whole tower is the accent."""
    return _temp_tower(range(TEMP_BLOCKS))


def calib_temp_overhang(k, n):
    """The overhang model: a row of small inverted-L pieces, one per temperature, their arms overhanging."""
    count, pitch = 4, 0.52

    def scene(p):
        items = [bed(p)]
        for i in range(count):
            cx = -0.78 + pitch * i
            post = sd_box(p, (cx, 0.0, 0.33), (0.07, 0.16, 0.33), 0.006)
            arm = sd_box(p, (cx + 0.15, 0.0, 0.61), (0.22, 0.16, 0.05), 0.006)
            items += [(post, M_PART), (arm, M_ACCENT)]
        return union(*items)
    return _still(scene, LOW_CAM)


# --- max volumetric speed: an E-shaped wall, printed as one spiral, faster with height ---------------

SPEED_H, SPEED_BANDS = 1.3, 10


def _speed(kind):
    band = SPEED_H / SPEED_BANDS
    accent = _which(kind, SPEED_BANDS)

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d2 = sd_segment2(x, y, (-0.72, -0.5), (-0.72, 0.5))
        for yy in (-0.5, 0.0, 0.5):
            d2 = np.minimum(d2, sd_segment2(x, y, (-0.72, yy), (0.72, yy)))
        d = extrude(d2 - 0.07, 0.0, SPEED_H)(p)
        return union(bed(p), (d, _bands(p[:, 2], band, SPEED_BANDS, accent)))
    return _still(scene, TOWER_CAM)


def calib_speed_start(k, n):
    return _speed("start")


def calib_speed_end(k, n):
    return _speed("end")


def calib_speed_step(k, n):
    return _speed("step")


# --- VFA: thin fins at several angles, one speed per band --------------------------------------------

VFA_H, VFA_BANDS = 1.3, 8


def _vfa(kind):
    band = VFA_H / VFA_BANDS
    accent = _which(kind, VFA_BANDS)
    fins = [math.radians(a) for a in (15.0, 75.0, 135.0, 195.0, 255.0, 315.0)]

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d2 = np.full(len(p), np.inf)
        for a in fins:
            d2 = np.minimum(d2, sd_segment2(x, y, (0.0, 0.0), (0.72 * math.cos(a), 0.72 * math.sin(a))))
        d = extrude(d2 - 0.025, 0.0, VFA_H)(p)
        return union(bed(p), (d, _bands(p[:, 2], band, VFA_BANDS, accent)))
    return _still(scene, TOWER_CAM)


def calib_vfa_start(k, n):
    return _vfa("start")


def calib_vfa_end(k, n):
    return _vfa("end")


def calib_vfa_step(k, n):
    return _vfa("step")


# --- retraction: two thin towers on a plate, the nozzle travelling between them ----------------------

RETR_H, RETR_BANDS = 1.3, 10


def _retraction(kind):
    band = RETR_H / RETR_BANDS
    accent = _which(kind, RETR_BANDS)

    def scene(p):
        z = p[:, 2]
        plate = sd_box(p, (0.0, 0.0, 0.025), (0.72, 0.2, 0.025), 0.01)
        towers = np.minimum(sd_cylinder(p, (-0.5, 0.0), 0.11, 0.0, RETR_H, 0.01),
                            sd_cylinder(p, (0.5, 0.0), 0.11, 0.0, RETR_H, 0.01))
        return union(bed(p), (plate, M_PART2), (towers, _bands(z, band, RETR_BANDS, accent)))
    return _still(scene, TOWER_CAM)


def calib_retraction_start(k, n):
    return _retraction("start")


def calib_retraction_end(k, n):
    return _retraction("end")


def calib_retraction_step(k, n):
    return _retraction("step")


# --- input shaping and cornering towers --------------------------------------------------------------

RING_H, RING_BANDS = 0.8, 8
RING_C = (-0.62, -0.62)  # the outer corner of the L
RING_ARM, RING_T = 1.35, 0.035


def _ringing_d(p):
    """The ringing tower: two long thin walls meeting at a corner, an L seen from above."""
    x, y = p[:, 0], p[:, 1]
    cx, cy = RING_C
    d2 = np.minimum(sd_segment2(x, y, (cx, cy), (cx + RING_ARM, cy)), sd_segment2(x, y, (cx, cy), (cx, cy + RING_ARM)))
    return extrude(d2 - RING_T, 0.0, RING_H)(p)


def _ringing(mat_fn):
    def scene(p):
        return union(bed(p), (_ringing_d(p), mat_fn(p)))
    return _still(scene, LOW_CAM)


def calib_ringing_tower(k, n):
    """The walls just past the corner are where the ripples show, so they are the accent."""
    cx, cy = RING_C

    def mat(p):
        x, y = p[:, 0], p[:, 1]
        near = ((x - cx > 0.15) & (x - cx < 0.6)) | ((y - cy > 0.15) & (y - cy < 0.6))
        return np.where(near, M_ACCENT, M_PART)
    return _ringing(mat)


def calib_ringing_start(k, n):
    return _ringing(lambda p: _bands(p[:, 2], RING_H / RING_BANDS, RING_BANDS, [0]))


def calib_ringing_end(k, n):
    return _ringing(lambda p: _bands(p[:, 2], RING_H / RING_BANDS, RING_BANDS, [RING_BANDS - 1]))


def calib_fast_tower(k, n):
    """The fast tower: a small block, one corner cut off; its corners are where the ripples show."""
    half, h = 0.42, 1.0

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d = sd_box(p, (0.0, 0.0, h * 0.5), (half, half, h * 0.5), 0.01)
        cut = (x - y - 1.7 * half) / math.sqrt(2.0)  # the front right edge, chamfered
        d = np.maximum(d, cut)
        corner = (np.abs(np.abs(x) - half) < 0.07) & (np.abs(np.abs(y) - half) < 0.07)
        chamfer = cut > -0.07
        mat = np.where(corner | chamfer, M_ACCENT, M_PART)
        return union(bed(p), (d, mat))
    return _still(scene, LOW_CAM)


def calib_scv(k, n):
    """SCV-V2: a zigzag wall, its sharp turns taken at the cornering limit being tested."""
    teeth, h = 8, 0.9
    xs = np.linspace(-0.85, 0.85, teeth + 1)
    pts = [(float(xx), 0.28 if i % 2 == 0 else -0.28) for i, xx in enumerate(xs)]

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d2 = np.full(len(p), np.inf)
        tip = np.full(len(p), np.inf)
        for a, b in zip(pts, pts[1:]):
            d2 = np.minimum(d2, sd_segment2(x, y, a, b))
        for c in pts:
            tip = np.minimum(tip, np.hypot(x - c[0], y - c[1]))
        d = extrude(d2 - 0.03, 0.0, h)(p)
        return union(bed(p), (d, np.where(tip < 0.1, M_ACCENT, M_PART)))
    return _still(scene, LOW_CAM)


# --- flow ratio: flat blocks side by side, one flow ratio each ---------------------------------------


def _plates(cells, size, best):
    """Thin blocks at grid cells (column, row); the smoothest one, `best`, in the accent colour."""
    gap = 0.06
    w, d = size
    cols = max(c for c, _ in cells) + 1
    rows = max(r for _, r in cells) + 1
    x0 = -0.5 * (cols * w + (cols - 1) * gap) + 0.5 * w
    y0 = -0.5 * (rows * d + (rows - 1) * gap) + 0.5 * d

    def scene(p):
        items = [bed(p)]
        for i, (c, r) in enumerate(cells):
            centre = (x0 + c * (w + gap), y0 + r * (d + gap), 0.03)
            items.append((sd_box(p, centre, (0.5 * w, 0.5 * d, 0.03), 0.008), M_ACCENT if i == best else M_PART))
        return union(*items)
    return _still(scene, FLAT_CAM)


def calib_flow_pass1(k, n):
    return _plates([(c, r) for r in range(3) for c in range(3)], (0.5, 0.38), 4)


def calib_flow_pass2(k, n):
    return _plates([(c, r) for r in range(3) for c in range(3)] + [(1, 3)], (0.46, 0.34), 7)


def calib_flow_yolo(k, n):
    cells = [(c, r) for r in range(3) for c in range(4) if (c, r) != (0, 0)]
    return _plates(cells, (0.4, 0.4), 5)


def calib_flow_yolo_fine(k, n):
    return _plates([(c, r) for r in range(4) for c in range(4)], (0.36, 0.36), 9)


# --- pressure advance: tower, lines and pattern, the value of one band, line or chevron --------------

PA_TOWER_BANDS = 10
PA_LINES = 9
PA_CHEVRONS = 4


def _pa_tower(kind):
    half, wall, h = 0.45, 0.045, 1.25
    band = h / PA_TOWER_BANDS
    accent = _which(kind, PA_TOWER_BANDS)

    def scene(p):
        outer = sd_box(p, (0.0, 0.0, h * 0.5), (half, half, h * 0.5))
        inner = sd_box(p, (0.0, 0.0, h * 0.5), (half - wall, half - wall, h * 0.5 + 0.1))
        d = np.maximum(outer, -inner)
        return union(bed(p), (d, _bands(p[:, 2], band, PA_TOWER_BANDS, accent)))
    return _still(scene, LOW_CAM)


def _pa_line(kind):
    x0, x1, xa, xb, th = -0.85, 0.85, -0.42, 0.42, 0.03
    accent = _which(kind, PA_LINES)

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        items = [bed(p)]
        for i in range(PA_LINES):  # the first line is at the front
            yy = -0.78 + 1.56 * i / (PA_LINES - 1)
            d2 = np.minimum(np.minimum(sd_segment2(x, y, (x0, yy), (xa, yy)) - 0.022,
                                       sd_segment2(x, y, (xa, yy), (xb, yy)) - 0.034),
                            sd_segment2(x, y, (xb, yy), (x1, yy)) - 0.022)
            items.append((extrude(d2, 0.0, th)(p), M_ACCENT if i in accent else M_PART))
        frame2 = np.abs(np.maximum(np.abs(x) - 1.02, np.abs(y) - 0.95)) - 0.025
        items.append((extrude(frame2, 0.0, th)(p), M_PART2))
        return union(*items)
    return _still(scene, LOW_CAM)


def _pa_pattern(kind):
    walls, arm, spacing, gap, w, h = 3, 0.26, 0.62, 0.06, 0.022, 0.16
    accent = _which(kind, PA_CHEVRONS)

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        items = [bed(p)]
        for i in range(PA_CHEVRONS):  # the first chevron is on the left
            cx = -0.93 + spacing * i
            d2 = np.full(len(p), np.inf)
            for j in range(walls):
                ty = 0.12 + gap * j * math.sqrt(2.0)
                d2 = np.minimum(d2, sd_segment2(x, y, (cx, ty), (cx - arm, ty + arm)) - w)
                d2 = np.minimum(d2, sd_segment2(x, y, (cx, ty), (cx + arm, ty + arm)) - w)
            items.append((extrude(d2, 0.0, h)(p), M_ACCENT if i in accent else M_PART))
        return union(*items)
    return _still(scene, LOW_CAM)


def calib_pa_tower_start(k, n):
    return _pa_tower("start")


def calib_pa_tower_end(k, n):
    return _pa_tower("end")


def calib_pa_tower_step(k, n):
    return _pa_tower("step")


def calib_pa_line_start(k, n):
    return _pa_line("start")


def calib_pa_line_end(k, n):
    return _pa_line("end")


def calib_pa_line_step(k, n):
    return _pa_line("step")


def calib_pa_pattern_start(k, n):
    return _pa_pattern("start")


def calib_pa_pattern_end(k, n):
    return _pa_pattern("end")


def calib_pa_pattern_step(k, n):
    return _pa_pattern("step")


# name -> frame count (0 = a still, written as PNG)
FRAMES = {name: 0 for name in [
    "calib_temp_start", "calib_temp_end", "calib_temp_standard", "calib_temp_overhang",
    "calib_speed_start", "calib_speed_end", "calib_speed_step",
    "calib_vfa_start", "calib_vfa_end", "calib_vfa_step",
    "calib_retraction_start", "calib_retraction_end", "calib_retraction_step",
    "calib_ringing_tower", "calib_ringing_start", "calib_ringing_end", "calib_fast_tower", "calib_scv",
    "calib_flow_pass1", "calib_flow_pass2", "calib_flow_yolo", "calib_flow_yolo_fine",
    "calib_pa_tower_start", "calib_pa_tower_end", "calib_pa_tower_step",
    "calib_pa_line_start", "calib_pa_line_end", "calib_pa_line_step",
    "calib_pa_pattern_start", "calib_pa_pattern_end", "calib_pa_pattern_step",
]}
