"""Tooltips of the main window and the general dialogs: the plate settings dialog (print sequence,
filament sequence, spiral vase) and the pressure advance calibration dialog (its three test methods).
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import (ACCENT, BED, GIF_SIZE, GRID, M_ACCENT, M_PART, M_PART2, SIZE, SS, Camera, Overlay, bed,
                     extrude, render, sd_box, sd_cylinder, sd_segment2, union)

OUT = "app_ui"  # resources/images/<OUT>

NOZZLE = (205, 210, 210)
NOZZLE_EDGE = (30, 32, 34)


def _rgb8(c):
    return tuple(int(round(v * 255.0)) for v in c)


def _nozzle(o, tip, size=1.0):
    """A small hot end over the point being printed, drawn flat like the pointer."""
    x, y = o.pt(tip)
    k = 9.0 * SS * size
    o.draw.polygon([(x, y), (x - 0.45 * k, y - 0.7 * k), (x + 0.45 * k, y - 0.7 * k)], fill=NOZZLE, outline=NOZZLE_EDGE,
                   width=max(1, SS))
    o.draw.rectangle([x - 0.95 * k, y - 1.9 * k, x + 0.95 * k, y - 0.7 * k], fill=NOZZLE, outline=NOZZLE_EDGE,
                     width=max(1, SS))


def _layer_ridges(z, lh, depth):
    """Pulls the surface in at every layer boundary, so the printed layers read as layers."""
    return depth * (0.5 + 0.5 * np.cos(2.0 * math.pi * z / lh))


# --- print sequence: by layer vs by object ---------------------------------------------------------

SEQ_LAYERS = 7
SEQ_LH = 0.14
SEQ_X = (-0.58, 0.58)
SEQ_R = 0.36


def _print_sequence(k, n, by_object):
    total = 2 * SEQ_LAYERS
    if by_object:
        order = [(obj, layer) for obj in range(2) for layer in range(SEQ_LAYERS)]
    else:
        order = [(obj, layer) for layer in range(SEQ_LAYERS) for obj in range(2)]
    # One layer per frame, then the finished plate held for the rest of the loop.
    done = order[: k + 1] if k < total else order
    current = order[k] if k < total else None
    heights = [sum(1 for o_, _ in done if o_ == obj) * SEQ_LH for obj in range(2)]

    def scene(p):
        items = [bed(p)]
        for obj in range(2):
            h = heights[obj]
            if h <= 0.0:
                continue
            d = sd_cylinder(p, (SEQ_X[obj], 0.0), SEQ_R, 0.0, h, 0.01) + _layer_ridges(p[:, 2], SEQ_LH, 0.008)
            mat = M_PART
            if current is not None and current[0] == obj:
                mat = np.where(p[:, 2] > h - SEQ_LH + 0.004, M_ACCENT, M_PART)
            items.append((d, mat))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.4)
    o = Overlay(render(scene, cam), cam)
    if current is not None:
        obj = current[0]
        _nozzle(o, (SEQ_X[obj] + 0.12, -0.12, heights[obj] + 0.03))
    return o.finish(GIF_SIZE)


def print_seq_by_layer(k, n):
    return _print_sequence(k, n, by_object=False)


def print_seq_by_object(k, n):
    return _print_sequence(k, n, by_object=True)


# --- filament sequence within a layer: a top view, drawn flat --------------------------------------

FILAMENTS = [_rgb8(ACCENT), (196, 140, 78), (112, 128, 196)]


def _region_masks(px):
    """The three colour regions of one layer of a part: a left band, a disc and the rest."""
    x0, y0, x1, y1 = int(0.17 * px), int(0.12 * px), int(0.83 * px), int(0.70 * px)
    split = int(0.42 * px)
    rad = int(0.035 * px)

    outline = Image.new("L", (px, px), 0)
    ImageDraw.Draw(outline).rounded_rectangle([x0, y0, x1, y1], radius=rad, fill=255)
    left = Image.new("L", (px, px), 0)
    ImageDraw.Draw(left).rectangle([0, 0, split, px], fill=255)
    disc = Image.new("L", (px, px), 0)
    cx, cy, r = int(0.63 * px), int(0.41 * px), int(0.15 * px)
    ImageDraw.Draw(disc).ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)

    a = np.asarray(outline) > 0
    lft = np.asarray(left) > 0
    dsc = np.asarray(disc) > 0
    gap = int(0.012 * px)  # a thin parting line between regions, as the walls of each colour leave
    m1 = a & lft
    m2 = a & dsc & ~lft
    m3 = a & ~lft & ~dsc
    return [m1, m2, m3], gap


def _fill_lines(mask, frac, spacing, width, colour, draw_layer):
    """Diagonal infill lines inside `mask`, the first `frac` of them, in print order."""
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return
    s = xs + ys  # 45 degree lines: x + y = const
    lo, hi = s.min(), s.max()
    count = max(1, int((hi - lo) / spacing))
    shown = int(math.ceil(frac * count - 1e-9))
    if shown <= 0:
        return
    lines = Image.new("L", mask.shape[::-1], 0)
    d = ImageDraw.Draw(lines)
    px = mask.shape[0]
    for i in range(shown):
        c = lo + (i + 0.5) * spacing
        d.line([(c, 0), (c - px, px)], fill=255, width=width)
    m = (np.asarray(lines) > 0) & mask
    draw_layer[m] = colour


def _contour(mask):
    inner = mask.copy()
    inner[1:, :] &= mask[:-1, :]
    inner[:-1, :] &= mask[1:, :]
    inner[:, 1:] &= mask[:, :-1]
    inner[:, :-1] &= mask[:, 1:]
    return mask & ~inner


def _dilate(mask, r):
    out = mask.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(mask, dy, axis=0), dx, axis=1)
    return out


def filament_seq(k, n):
    px = GIF_SIZE * SS
    masks, gap = _region_masks(px)
    # Shrink each region a little so a parting line stays between them.
    shrunk = []
    for m in masks:
        edge = _dilate(_contour(m), gap // 2)
        shrunk.append(m & ~edge)

    bg = (BED * 0.92 * 255.0).astype(np.uint8)
    img = np.empty((px, px, 3), np.uint8)
    img[:] = bg
    # The bed grid, as in the 3D pictures.
    grid = _rgb8(GRID)
    step = px // 8
    for i in range(0, px, step):
        img[i:i + SS, :] = grid
        img[:, i:i + SS] = grid

    per = 6  # frames per region
    for r, m in enumerate(shrunk):
        frac = min(max((k - r * per + 1) / float(per), 0.0), 1.0)
        dim = tuple(int(0.18 * c + 0.82 * b) for c, b in zip(FILAMENTS[r], bg))
        img[m] = (np.array(dim) * 0.85 + bg * 0.15).astype(np.uint8)
        _fill_lines(m, frac, spacing=7 * SS, width=int(4.4 * SS), colour=FILAMENTS[r], draw_layer=img)
        if frac > 0.0:
            img[_dilate(_contour(m), 1)] = FILAMENTS[r]
        else:
            img[_contour(m)] = tuple(int(0.45 * c + 0.55 * b) for c, b in zip(FILAMENTS[r], bg))

    im = Image.fromarray(img)
    d = ImageDraw.Draw(im)
    # The order chips under the part, as the dialog's drag-to-reorder row shows them.
    active = min(k // per, 2) if k < 3 * per else None
    chip, space = int(0.13 * px), int(0.06 * px)
    left = (px - (3 * chip + 2 * space)) // 2
    top = int(0.78 * px)
    for r in range(3):
        x = left + r * (chip + space)
        lift = int(0.03 * px) if r == active else 0
        box = [x, top - lift, x + chip, top - lift + chip]
        d.rounded_rectangle(box, radius=int(0.02 * px), fill=FILAMENTS[r],
                            outline=(235, 242, 242) if r == active else (30, 32, 34), width=(3 if r == active else 1) * SS)
    return np.asarray(im.resize((GIF_SIZE, GIF_SIZE), Image.LANCZOS))


# --- spiral vase: stacked layers with a seam vs one helical wall -----------------------------------

VASE_H = 1.30
VASE_LH = 0.13
VASE_WALL = 0.035
VASE_BOTTOM = 0.08
SEAM_ANGLE = math.radians(-62.0)


def _vase_radius(z):
    return 0.42 + 0.10 * np.sin(z / VASE_H * math.pi * 1.1 + 0.2)


def _vase(spiral):
    def scene(p):
        z = p[:, 2]
        r = np.hypot(p[:, 0], p[:, 1])
        theta = np.arctan2(p[:, 1], p[:, 0])
        rz = _vase_radius(z)
        wall = np.abs(r - rz + VASE_WALL * 0.5) - VASE_WALL * 0.5
        if spiral:
            # The wall ends on the helix, so the rim climbs once around instead of ending level.
            top = VASE_H - VASE_LH * (1.0 - (theta / (2.0 * math.pi)) % 1.0)
        else:
            top = VASE_H
        dz = np.maximum(-z, z - top)
        bottom = np.maximum(r - rz, np.maximum(-z, z - VASE_BOTTOM))
        shell = np.minimum(np.maximum(wall, dz), bottom) * 0.8
        if spiral:
            phase = z / VASE_LH - theta / (2.0 * math.pi)
            ridge = 0.012 * (0.5 + 0.5 * np.cos(2.0 * math.pi * phase))
            mat = np.where((z > VASE_BOTTOM + 0.01) & (np.abs((phase % 1.0) - 0.5) < 0.17), M_ACCENT, M_PART)
            return union(bed(p), (shell + ridge * (z > VASE_BOTTOM), mat))
        ridge = _layer_ridges(z, VASE_LH, 0.012)
        items = [bed(p), (shell + ridge, M_PART)]
        # The seam: where each layer starts and ends, a small blob in a line up the side.
        zs = np.arange(VASE_LH, VASE_H + 1e-6, VASE_LH)
        zi = np.clip(np.round(z / VASE_LH), 1, len(zs)) * VASE_LH
        rs = _vase_radius(zi)
        c = np.stack([rs * math.cos(SEAM_ANGLE), rs * math.sin(SEAM_ANGLE), zi], axis=1)
        items.append((np.linalg.norm(p - c, axis=1) - 0.021, M_ACCENT))
        return union(*items)

    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def spiral_vase_off(k, n):
    return _vase(False)


def spiral_vase_on(k, n):
    return _vase(True)


# --- pressure advance tests: tower, lines, pattern (one camera for the three) ----------------------

PA_CAM = dict(target=(0.0, 0.0, 0.35), extent=2.7)


def pa_tower(k, n):
    half, wall, h, lh, band = 0.45, 0.045, 1.25, 0.05, 0.125

    def scene(p):
        outer = sd_box(p, (0.0, 0.0, h * 0.5), (half, half, h * 0.5))
        inner = sd_box(p, (0.0, 0.0, h * 0.5), (half - wall, half - wall, h * 0.5 + 0.1))
        d = np.maximum(outer, -inner) + _layer_ridges(p[:, 2], lh, 0.004)
        # The value changes every band up the tower; the corners are where it shows.
        mat = np.where((p[:, 2] // band) % 2 == 0, M_PART, M_PART2)
        corner = (np.abs(p[:, 0]) > half - 0.11) & (np.abs(p[:, 1]) > half - 0.11)
        mat = np.where(corner, M_ACCENT, mat)
        return union(bed(p), (d, mat))

    cam = Camera(SIZE, **PA_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def pa_line(k, n):
    lines = 9
    x0, x1 = -0.85, 0.85
    xa, xb = -0.42, 0.42  # the fast middle third
    th = 0.03

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d2 = np.full(len(p), np.inf)
        for i in range(lines):
            yy = -0.78 + 1.56 * i / (lines - 1)
            d2 = np.minimum(d2, sd_segment2(x, y, (x0, yy), (xa, yy)) - 0.022)
            d2 = np.minimum(d2, sd_segment2(x, y, (xa, yy), (xb, yy)) - 0.034)
            d2 = np.minimum(d2, sd_segment2(x, y, (xb, yy), (x1, yy)) - 0.022)
        lines_d = extrude(d2, 0.0, th)(p)
        # The frame printed around the lines.
        fx, fy = 1.02, 0.95
        frame2 = np.abs(np.maximum(np.abs(x) - fx, np.abs(y) - fy)) - 0.025
        frame_d = extrude(frame2, 0.0, th)(p)
        return union(bed(p), (lines_d, M_ACCENT), (frame_d, M_PART))

    cam = Camera(SIZE, **PA_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def pa_pattern(k, n):
    # Nested chevrons side by side, one per value, their tips towards the front.
    count, walls = 4, 3
    arm, spacing, gap, w = 0.26, 0.62, 0.06, 0.022
    h = 0.16

    def scene(p):
        x, y = p[:, 0], p[:, 1]
        d2 = np.full(len(p), np.inf)
        for i in range(count):
            cx = -0.93 + spacing * i
            for j in range(walls):
                ty = 0.12 + gap * j * math.sqrt(2.0)
                d2 = np.minimum(d2, sd_segment2(x, y, (cx, ty), (cx - arm, ty + arm)) - w)
                d2 = np.minimum(d2, sd_segment2(x, y, (cx, ty), (cx + arm, ty + arm)) - w)
        d = extrude(d2, 0.0, h)(p) + _layer_ridges(p[:, 2], h / 3.0, 0.003)
        return union(bed(p), (d, M_ACCENT))

    cam = Camera(SIZE, **PA_CAM)
    return Overlay(render(scene, cam), cam).finish(SIZE)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "print_seq_by_layer": 20,
    "print_seq_by_object": 20,
    "filament_seq": 21,
    "spiral_vase_off": 0,
    "spiral_vase_on": 0,
    "pa_tower": 0,
    "pa_line": 0,
    "pa_pattern": 0,
}
