"""The pictures in the tooltips of the filament settings (SettingTipsFilament.cpp) and of the filament
dialogs: what a setting does to the filament, the fan or the printed part.

Curves and the inside of the hotend are flat diagrams drawn with PIL in the renderer's palette; the
printed part, the bead and the travel moves are ray-marched scenes like the gizmo pictures.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from . import render as _render
from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "settings/filament"  # resources/images/<OUT>


# --- palette, as 8-bit colours --------------------------------------------------------------------


def _rgb(c, a=None):
    t = tuple(int(round(float(v) * 255.0)) for v in np.clip(c, 0.0, 1.0))
    return t if a is None else t + (a,)


BG = _rgb(BED * 0.75)  # what the ray marcher leaves where a ray hits nothing
C_GRID = _rgb(GRID)
C_AXIS = (128, 135, 140)
C_PART = _rgb(PART)
C_PART2 = _rgb(PART2)
C_DARK = (72, 77, 82)
C_ACCENT = _rgb(ACCENT)
C_ACCENT_LIGHT = (70, 205, 186)

# Bands from the part's grey to the accent, for a mixed filament's gradient: a gradient prints as layers
# of slowly changing mix, so bands are what it looks like, not a smooth ramp. Appended to the renderer's
# materials so the scenes can name them like any other.
N_BANDS = 12
M_BAND0 = _render.MATERIALS.shape[0]
_render.MATERIALS = np.vstack([_render.MATERIALS, [PART + (ACCENT - PART) * i / (N_BANDS - 1) for i in range(N_BANDS)]])


# --- flat diagrams -------------------------------------------------------------------------------


class Flat:
    """A 2D drawing in unit coordinates (x right, y down), drawn at twice the size and shrunk."""

    def __init__(self, size):
        self.size = size
        self.px = size * SS
        self.image = Image.new("RGB", (self.px, self.px), BG)
        self.draw = ImageDraw.Draw(self.image, "RGBA")

    def p(self, x, y):
        return (x * self.px, y * self.px)

    def w(self, width):
        return max(1, int(round(width * SS * self.size / 256.0)))

    def rect(self, x0, y0, x1, y1, fill, outline=None, width=1.0, r=0.0):
        box = [self.p(x0, y0), self.p(x1, y1)]
        if r > 0.0:
            self.draw.rounded_rectangle(box, radius=r * self.px, fill=fill, outline=outline, width=self.w(width))
        else:
            self.draw.rectangle(box, fill=fill, outline=outline, width=self.w(width))

    def poly(self, pts, fill, outline=None, width=1.0):
        self.draw.polygon([self.p(*q) for q in pts], fill=fill, outline=outline, width=self.w(width) if outline else 1)

    def line(self, pts, colour, width=1.5):
        self.draw.line([self.p(*q) for q in pts], fill=colour, width=self.w(width), joint="curve")

    def dashed(self, a, b, colour, width=1.2, dash=0.025):
        length = math.hypot(b[0] - a[0], b[1] - a[1])
        steps = max(1, int(length / dash))
        for i in range(0, steps, 2):
            t0, t1 = i / steps, min((i + 1) / steps, 1.0)
            self.line([(a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0),
                       (a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1)], colour, width)

    def ellipse(self, cx, cy, rx, ry, fill, outline=None, width=1.0):
        self.draw.ellipse([self.p(cx - rx, cy - ry), self.p(cx + rx, cy + ry)], fill=fill, outline=outline,
                          width=self.w(width) if outline else 1)

    def arrow(self, a, b, colour, width=2.0, head=0.035):
        v = np.array(b, float) - np.array(a, float)
        length = np.linalg.norm(v)
        if length < 1e-6:
            return
        v /= length
        n = np.array([-v[1], v[0]])
        base = np.array(b) - v * head
        self.line([a, tuple(base)], colour, width)
        self.poly([tuple(b), tuple(base + n * head * 0.45), tuple(base - n * head * 0.45)], colour)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _bead(f, cx, cy, rx, ry, colour):
    """An extrusion seen end on: an oval, lighter on top where the light falls."""
    base = np.array(colour[:3], float)
    f.ellipse(cx, cy, rx, ry, tuple(int(c * 0.55) for c in base))
    f.ellipse(cx - rx * 0.06, cy - ry * 0.08, rx * 0.9, ry * 0.82, tuple(int(c) for c in base))
    f.ellipse(cx - rx * 0.25, cy - ry * 0.35, rx * 0.45, ry * 0.32, tuple(int(min(255, c * 1.18)) for c in base))


# --- cooling: the fan against the layer time, and over the first layers -------------------------
#
# GCode/CoolingBuffer.cpp: a layer shorter than slow_down_layer_time gets fan_max_speed, one between it
# and fan_cooling_layer_time an interpolated speed, and a longer one no fan, or fan_min_speed when
# reduce_fan_stop_start_freq is on.

_AX0, _AY0, _AX1, _AY1 = 0.14, 0.82, 0.88, 0.16  # chart area: origin bottom left


def _chart_axes(f):
    for i in range(1, 5):
        y = _AY0 + (_AY1 - _AY0) * i / 4.0
        f.line([(_AX0, y), (_AX1, y)], C_GRID, 1.0)
    f.arrow((_AX0, _AY0), (_AX1 + 0.05, _AY0), C_AXIS, 2.0)
    f.arrow((_AX0, _AY0), (_AX0, _AY1 - 0.05), C_AXIS, 2.0)


def _fan_curve(always_on):
    f = Flat(SIZE)
    _chart_axes(f)
    X = lambda t: _AX0 + (_AX1 - _AX0) * t
    Y = lambda v: _AY0 + (_AY1 - _AY0) * v
    t1, t2, vmax, vmin = 0.28, 0.62, 0.86, 0.34
    tail = vmin if always_on else 0.0
    pts = [(X(0.0), Y(vmax)), (X(t1), Y(vmax)), (X(t2), Y(vmin)), (X(t2), Y(tail)), (X(1.0), Y(tail))]
    f.poly(pts + [(X(1.0), Y(0.0)), (X(0.0), Y(0.0))], C_ACCENT + (70,))
    # The two layer times, and the two speeds they switch between.
    for t, v in ((t1, vmax), (t2, vmin)):
        f.dashed((X(t), Y(0.0)), (X(t), Y(v)), C_PART, 1.3)
        f.dashed((X(0.0), Y(v)), (X(t), Y(v)), C_PART, 1.3)
    f.line(pts[:3], C_ACCENT_LIGHT, 3.2)
    f.line(pts[2:4], C_ACCENT_LIGHT if always_on else C_ACCENT, 2.2)
    f.line(pts[3:], C_ACCENT_LIGHT, 3.2)
    for t, v in ((t1, vmax), (t2, vmin)):
        f.ellipse(X(t), Y(v), 0.017, 0.017, C_ACCENT_LIGHT, BG, 1.5)
    return f.finish()


def fan_curve(k, n):
    return _fan_curve(False)


def fan_curve_always_on(k, n):
    return _fan_curve(True)


def _layer_fans(speeds, colours):
    """The first layers of a print seen from the side, each with a bar for its fan speed: layer 0 at the bottom."""
    f = Flat(SIZE)
    count = len(speeds)
    y0, y1 = 0.86, 0.12
    h = (y0 - y1) / count
    for i, (s, c) in enumerate(zip(speeds, colours)):
        top = y0 - (i + 1) * h + h * 0.12
        bottom = y0 - i * h - h * 0.12
        # The layer itself.
        f.rect(0.08, top, 0.36, bottom, C_PART2, r=0.012)
        f.rect(0.08, top, 0.36, top + (bottom - top) * 0.35, C_PART + (90,), r=0.012)
        # Its fan speed.
        f.rect(0.44, top, 0.92, bottom, C_GRID, r=0.012)
        if s > 0.0:
            f.rect(0.44, top, 0.44 + 0.48 * s, bottom, c, r=0.012)
        elif c != C_PART:
            f.rect(0.44, top, 0.92, bottom, None, c, 1.4, r=0.012)
    f.line([(0.05, y0 + 0.012), (0.39, y0 + 0.012)], C_AXIS, 2.0)  # the bed
    return f.finish()


def fan_ramp(k, n):
    # close_fan_the_first_x_layers = 2, full_fan_speed_layer = 6: off, then ramped up to the full speed.
    first, full = 2, 6
    speeds, colours = [], []
    for i in range(10):
        if i < first:
            speeds.append(0.0); colours.append(C_ACCENT)
        elif i + 1 < full:
            speeds.append((i + 1 - first) / float(full - first)); colours.append(C_ACCENT)
        else:
            speeds.append(1.0); colours.append(C_ACCENT if i + 1 == full else C_PART)
    return _layer_fans(speeds, colours)


def fan_first_layer(k, n):
    # initial_layer_fan_speed on layer 0, then the ramp from it up to full_fan_speed_layer = 5.
    anchor, full = 0.3, 5
    speeds = [anchor] + [anchor + (1.0 - anchor) * i / (full - 1) if i < full else 1.0 for i in range(1, 10)]
    colours = [C_ACCENT_LIGHT] + [C_ACCENT if i < full else C_PART for i in range(1, 10)]
    return _layer_fans(speeds, colours)


# --- overhang fan threshold ----------------------------------------------------------------------
#
# A wall leaning out, cut through: each layer's outer line overhangs the one below by a growing part
# of its width. The lines past the threshold get the overhang fan.

_OVERHANG = [0.0, 0.0, 0.05, 0.15, 0.3, 0.6, 0.85, 0.97]


def _overhang(threshold):
    f = Flat(SIZE)
    rx, ry = 0.085, 0.043
    y = 0.84
    x = 0.24
    f.line([(0.06, y + ry + 0.012), (0.94, y + ry + 0.012)], C_AXIS, 2.0)
    prev = None
    for i, frac in enumerate(_OVERHANG):
        x += frac * 2.0 * rx
        if prev is not None:
            # Where the line below ends: past it, this line has nothing under it.
            f.dashed((prev + rx, y + 1.6 * ry), (prev + rx, y - 1.6 * ry), (215, 222, 222, 210), 1.4, 0.014)
        fan = frac > threshold or threshold == 0.0
        _bead(f, x, y, rx, ry, C_ACCENT if fan else C_PART)
        prev = x
        y -= 2.0 * ry * 0.92
    return f.finish()


def overhang_fan_threshold_0(k, n):
    return _overhang(0.0)


def overhang_fan_threshold_10(k, n):
    return _overhang(0.10)


def overhang_fan_threshold_25(k, n):
    return _overhang(0.25)


def overhang_fan_threshold_50(k, n):
    return _overhang(0.50)


def overhang_fan_threshold_75(k, n):
    return _overhang(0.75)


def overhang_fan_threshold_95(k, n):
    return _overhang(0.95)


# --- the hotend at a filament change: ramming, unloading, cooling moves, stamping -----------------
#
# GCode/WipeTower2.cpp, toolchange_Unload(): the filament is rammed out on the wipe tower, pulled up to
# the cooling tube (fast at first, slowing down), moved down and up the cooling tube a few times at a
# rising speed, optionally pushed back toward the nozzle between those moves ("stamping"), and parked.

_TUBE_TOP, _TUBE_BOTTOM = 0.12, 0.44
_TUBE_CENTRE = 0.5 * (_TUBE_TOP + _TUBE_BOTTOM)
_NOZZLE_TIP = 0.80


def _hotend(f):
    cx = 0.5
    # Barrel and heat sink fins around the cooling tube.
    f.rect(cx - 0.065, 0.0, cx + 0.065, 0.58, C_DARK)
    y = _TUBE_TOP - 0.01
    while y < _TUBE_BOTTOM:
        f.rect(cx - 0.21, y, cx + 0.21, y + 0.024, C_PART2, r=0.006)
        y += 0.052
    f.rect(cx - 0.045, 0.44, cx + 0.045, 0.58, C_PART2)  # heat break
    f.rect(cx - 0.19, 0.57, cx + 0.15, 0.72, (122, 110, 108), r=0.01)  # heater block
    f.poly([(cx - 0.07, 0.72), (cx + 0.07, 0.72), (cx + 0.022, _NOZZLE_TIP), (cx - 0.022, _NOZZLE_TIP)], C_PART2)
    f.rect(cx - 0.028, -0.02, cx + 0.028, _NOZZLE_TIP - 0.005, (24, 25, 27))  # the bore
    f.line([(0.06, 0.86), (0.94, 0.86)], C_AXIS, 2.0)  # top of the wipe tower


def _filament(f, tip, travel, shape="point", melt=False):
    """The filament in the bore, its end at `tip`; `travel` moves the stripes along with it."""
    cx, hw = 0.5, 0.021
    end = tip - (0.035 if shape == "point" else 0.012)
    f.rect(cx - hw, -0.05, cx + hw, end, C_ACCENT)
    if shape == "point":
        f.poly([(cx - hw, end), (cx + hw, end), (cx + 0.004, tip), (cx - 0.004, tip)], C_ACCENT)
    else:
        f.ellipse(cx, end, hw, 0.012, C_ACCENT)
    if melt:  # molten plastic filling the nozzle down to its opening
        f.poly([(cx - 0.028, 0.70), (cx + 0.028, 0.70), (cx + 0.012, _NOZZLE_TIP), (cx - 0.012, _NOZZLE_TIP)], C_ACCENT)
    # Marks on the filament, so a move of it shows.
    s = -0.05 + (travel % 0.06)
    while s < end - 0.01:
        if s > -0.04:
            f.line([(cx - hw, s), (cx + hw, s)], (18, 92, 84), 1.4)
        s += 0.06


def _tube_marks(f, extra=None):
    """Ticks beside the barrel at the cooling tube's ends and centre."""
    for y in (_TUBE_TOP, _TUBE_BOTTOM):
        f.line([(0.75, y), (0.80, y)], C_PART, 1.6)
    f.line([(0.775, _TUBE_TOP), (0.775, _TUBE_BOTTOM)], C_PART, 1.2)
    f.dashed((0.25, _TUBE_CENTRE), (0.82, _TUBE_CENTRE), C_PART + (140,), 1.0, 0.014)
    if extra is not None:
        f.line([(0.72, extra), (0.82, extra)], C_ACCENT_LIGHT, 2.2)


def _tri(u):
    """0 -> 1 -> 0 over each unit of u."""
    u = u % 1.0
    return 2.0 * u if u < 0.5 else 2.0 - 2.0 * u


def toolchange_unload(k, n):
    f = Flat(GIF_SIZE)
    _hotend(f)
    _tube_marks(f)
    bead = 0.0
    if k <= 5:  # ramming: pushed out ever faster, laying a line on the wipe tower
        t = k / 5.0
        tip, travel, shape, melt = _NOZZLE_TIP, 0.16 * t * t, "flat", True
        bead = 0.36 * t * t
    else:
        bead = 0.36
        if k <= 10:  # unloading: up to the cooling tube, fast first
            t = 1.0 - (1.0 - (k - 5) / 5.0) ** 2
            tip = _NOZZLE_TIP + (_TUBE_TOP - _NOZZLE_TIP) * t
        elif k <= 18:  # cooling moves: down the tube and back, twice, faster each time
            u = 2.0 * ((k - 10) / 8.0) ** 0.8
            tip = _TUBE_TOP + (_TUBE_BOTTOM - _TUBE_TOP) * _tri(u)
        else:  # parked
            tip = _TUBE_TOP - 0.06 * smooth((k - 18) / 2.0)
        travel, shape, melt = 0.16 + tip - _NOZZLE_TIP, "point", False
    if bead > 0.0:
        f.rect(0.5, 0.832, 0.5 + bead, 0.858, C_ACCENT, r=0.01)
    _filament(f, tip, travel, shape, melt)
    return f.finish()


def cooling_stamping(k, n):
    f = Flat(GIF_SIZE)
    _hotend(f)
    depth = _TUBE_CENTRE + 0.25  # the stamping distance, from the centre of the cooling tube
    stamping = 6 <= k <= 12
    _tube_marks(f, depth if stamping else None)
    if k <= 5:  # a cooling move
        tip = _TUBE_TOP + (_TUBE_BOTTOM - _TUBE_TOP) * _tri(k / 6.0)
    elif k <= 10:  # stamping: pushed down past the centre, slowly
        tip = _TUBE_TOP + (depth - _TUBE_TOP) * smooth((k - 6) / 4.0)
    elif k <= 12:  # and pulled back up
        tip = depth + (_TUBE_TOP - depth) * smooth((k - 10) / 2.0)
    elif k <= 18:  # the next cooling move
        tip = _TUBE_TOP + (_TUBE_BOTTOM - _TUBE_TOP) * _tri((k - 13) / 6.0)
    else:
        tip = _TUBE_TOP
    f.rect(0.5, 0.832, 0.86, 0.858, C_ACCENT, r=0.01)
    _filament(f, tip, tip, "point" if k < 10 else "flat")
    return f.finish()


# --- shrinkage compensation ----------------------------------------------------------------------
#
# The part is scaled up by the shrinkage when sliced (filament_shrink in XY, the Z one in height), and
# comes down to its designed size as it cools.

_BOX = np.array([0.55, 0.55, 0.42])


def _shrink(k, n, axes):
    grow = 0.18 * (1.0 - smooth(phase(k, 4, 9)))
    scale = np.array([1.0 + grow * axes[0], 1.0 + grow * axes[1], 1.0 + grow * axes[2]])
    half = _BOX * scale
    centre = np.array([0.0, 0.0, half[2]])
    scene = lambda p: union(bed(p), (sd_box(p, centre, half, 0.025), M_ACCENT))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.5)
    o = Overlay(render(scene, cam), cam)
    # The size the part was designed at; the edges behind it fainter.
    c0 = np.array([0.0, 0.0, _BOX[2]])
    signs = [np.array([sx, sy, sz]) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    for i, a in enumerate(signs):
        for b in signs[i + 1:]:
            if np.sum(a != b) != 1:
                continue
            axis = int(np.argmax(a != b))
            front = any(a[o2] * cam.c[o2] > 0.0 for o2 in range(3) if o2 != axis)
            o.line(c0 + _BOX * a, c0 + _BOX * b, CURSOR + ((230,) if front else (70,)), width=1.4 if front else 1.0)
    return o.finish(GIF_SIZE)


def shrink_xy(k, n):
    return _shrink(k, n, (1.0, 1.0, 0.0))


def shrink_z(k, n):
    return _shrink(k, n, (0.0, 0.0, 1.0))


# --- pressure advance: one bead round a corner, seen from above ----------------------------------

_PA_PATH = [np.array([-0.95, -0.55]), np.array([0.45, -0.55]), np.array([0.45, 0.85])]
_PA_R, _PA_H = 0.11, 0.07


def _pa_radius(s, kind):
    corner = 1.4  # arc length of the corner
    if kind == "low":  # too little: plastic piles up at the corner, the line after it runs thin
        return _PA_R * (1.0 + 0.75 * np.exp(-((s - corner) / 0.11) ** 2) - 0.32 * np.exp(-((s - corner - 0.32) / 0.13) ** 2))
    if kind == "high":  # too much: the corner starves, the line after it bulges a little
        return _PA_R * (1.0 - 0.55 * np.exp(-((s - corner) / 0.12) ** 2) + 0.22 * np.exp(-((s - corner - 0.34) / 0.12) ** 2))
    return np.full_like(s, _PA_R)


def _pressure_advance(kind):
    def scene(p):
        best_d, best_m = None, None
        s0 = 0.0
        for a, b in zip(_PA_PATH[:-1], _PA_PATH[1:]):
            e = b - a
            length = float(np.linalg.norm(e))
            t = np.clip(((p[:, 0] - a[0]) * e[0] + (p[:, 1] - a[1]) * e[1]) / (length * length), 0.0, 1.0)
            dxy = np.hypot(p[:, 0] - (a[0] + e[0] * t), p[:, 1] - (a[1] + e[1] * t))
            r = _pa_radius(s0 + t * length, kind)
            q = np.hypot(dxy / r, (p[:, 2] - _PA_H) / _PA_H)
            d = (q - 1.0) * np.minimum(r, _PA_H)
            off = np.abs(r - _PA_R) > 0.012
            m = np.where(off, M_ACCENT, M_PART)
            if best_d is None:
                best_d, best_m = d, m
            else:
                closer = d < best_d
                best_d, best_m = np.where(closer, d, best_d), np.where(closer, m, best_m)
            s0 += length
        return union(bed(p), (best_d, best_m))

    cam = Camera(SIZE, target=(-0.05, 0.1, 0.05), extent=2.3, az=25.0, el=58.0)
    return Overlay(render(scene, cam, step=0.6, steps=220), cam).finish(SIZE)


def pressure_advance_low(k, n):
    return _pressure_advance("low")


def pressure_advance_right(k, n):
    return _pressure_advance("right")


def pressure_advance_high(k, n):
    return _pressure_advance("high")


# --- the filament grouping dialog's swap button --------------------------------------------------


def _slot(panel, i):
    x0 = 0.06 if panel == 0 else 0.56
    return (x0 + 0.08 + 0.11 * i, 0.56)


def filament_map_swap(k, n):
    f = Flat(GIF_SIZE)
    for x0 in (0.06, 0.56):
        f.rect(x0, 0.40, x0 + 0.38, 0.72, (44, 47, 50), C_GRID, 1.5, r=0.03)
        # The nozzle the group prints with.
        cx = x0 + 0.19
        f.rect(cx - 0.07, 0.20, cx + 0.07, 0.29, C_PART2, r=0.01)
        f.poly([(cx - 0.04, 0.29), (cx + 0.04, 0.29), (cx + 0.012, 0.34), (cx - 0.012, 0.34)], C_PART2)
    # The swap button between them.
    f.arrow((0.462, 0.52), (0.542, 0.52), C_PART, 2.2, 0.026)
    f.arrow((0.538, 0.60), (0.458, 0.60), C_PART, 2.2, 0.026)
    # Out and back again, so the loop has no jump.
    if k < 10:
        t = smooth(phase(k, 3, 6))
    else:
        t = 1.0 - smooth(phase(k, 13, 6))
    blocks = [(0, i, C_ACCENT) for i in range(2)] + [(1, i, C_PART) for i in range(3)]
    for side, i, colour in blocks:
        a = _slot(side, i)
        b = _slot(1 - side, i)
        lift = (-0.2 if side == 0 else 0.2) * math.sin(math.pi * t)
        x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t + lift
        f.rect(x - 0.04, y - 0.04, x + 0.04, y + 0.04, colour, r=0.012)
    pressed = k in (2, 3, 12, 13)
    if pressed:
        f.ellipse(0.5, 0.56, 0.045, 0.045, None, C_ACCENT_LIGHT, 1.6)
    # The pointer on the button.
    x, y = 0.51, 0.585
    s = 0.075
    f.poly([(x, y), (x, y + 1.45 * s * 0.6), (x + 0.38 * s * 0.6, y + 1.05 * s * 0.6), (x + 0.62 * s * 0.6, y + 1.55 * s * 0.6),
            (x + 0.90 * s * 0.6, y + 1.40 * s * 0.6), (x + 0.66 * s * 0.6, y + 0.92 * s * 0.6), (x + 1.05 * s * 0.6, y + 0.84 * s * 0.6)],
           CURSOR, (30, 32, 34), 1.0)
    return f.finish()


# --- a mixed filament's gradient -----------------------------------------------------------------
#
# GCode/ToolOrdering.cpp: with the gradient on, the mix runs from one filament to the other over the
# layers of each object that print with it, or of each part with the per-part option.


def _band(z, z0, z1):
    t = np.clip((z - z0) / max(z1 - z0, 1e-6), 0.0, 0.999)
    return M_BAND0 + np.floor(t * N_BANDS).astype(int)


def _mixed_still(material_of):
    def scene(p):
        base = sd_box(p, (0.0, 0.0, 0.2), (0.62, 0.62, 0.2), 0.02)
        column = sd_cylinder(p, (0.0, 0.0), 0.36, 0.4, 1.35, 0.03)
        return union(bed(p), (np.minimum(base, column), material_of(p)))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.3)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def mixed_gradient_off(k, n):
    return _mixed_still(lambda p: np.full(len(p), M_BAND0 + N_BANDS // 2))


def mixed_gradient_on(k, n):
    return _mixed_still(lambda p: _band(p[:, 2], 0.0, 1.35))


def mixed_gradient_per_part(k, n):
    return _mixed_still(lambda p: np.where(p[:, 2] < 0.41, _band(p[:, 2], 0.0, 0.4), _band(p[:, 2], 0.4, 1.35)))


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "fan_curve": 0,
    "fan_curve_always_on": 0,
    "fan_ramp": 0,
    "fan_first_layer": 0,
    "overhang_fan_threshold_0": 0,
    "overhang_fan_threshold_10": 0,
    "overhang_fan_threshold_25": 0,
    "overhang_fan_threshold_50": 0,
    "overhang_fan_threshold_75": 0,
    "overhang_fan_threshold_95": 0,
    "toolchange_unload": 22,
    "cooling_stamping": 22,
    "shrink_xy": 18,
    "shrink_z": 18,
    "pressure_advance_low": 0,
    "pressure_advance_right": 0,
    "pressure_advance_high": 0,
    "filament_map_swap": 20,
    "mixed_gradient_off": 0,
    "mixed_gradient_on": 0,
    "mixed_gradient_per_part": 0,
}
