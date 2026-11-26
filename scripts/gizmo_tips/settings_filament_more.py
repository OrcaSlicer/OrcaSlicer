"""More pictures for the filament settings (SettingTipsFilament.cpp): flow ratio, diameter, the
temperatures, volumetric speed, adaptive pressure advance, the cooling slowdown, the auxiliary and
exhaust fans, and the multi-material tool change. Same palette and helpers as settings_filament.py,
whose folder it shares.
"""

import math

import numpy as np

from .render import ACCENT, CURSOR, GIF_SIZE, M_ACCENT, M_PART, M_PART2, PART, SIZE, Camera, Overlay, bed, render, \
    sd_box, smooth, union
from .settings_filament import (BG, C_ACCENT, C_ACCENT_LIGHT, C_AXIS, C_DARK, C_GRID, C_PART, C_PART2, Flat, _AX0, _AX1,
                                _AY0, _AY1, _NOZZLE_TIP, _TUBE_BOTTOM, _TUBE_TOP, _chart_axes, _filament, _hotend,
                                _tube_marks)

OUT = "settings/filament"  # resources/images/<OUT>

X = lambda t: _AX0 + (_AX1 - _AX0) * t  # chart coordinates, 0..1 along each axis
Y = lambda v: _AY0 + (_AY1 - _AY0) * v


# --- beads lying on the bed or on a layer: (start, end) in xy, half width r, half height h, base z ---


def _beads(p, beads):
    """Distance and material of the nearest of several straight beads with an oval section."""
    best_d, best_m = None, None
    for a, b, r, h, z0, m in beads:
        a, b = np.asarray(a, float), np.asarray(b, float)
        e = b - a
        length2 = float(e @ e)
        t = np.clip(((p[:, 0] - a[0]) * e[0] + (p[:, 1] - a[1]) * e[1]) / length2, 0.0, 1.0)
        dxy = np.hypot(p[:, 0] - (a[0] + e[0] * t), p[:, 1] - (a[1] + e[1] * t))
        q = np.hypot(dxy / r, (p[:, 2] - z0 - h) / h)
        d = (q - 1.0) * min(r, h)
        if best_d is None:
            best_d, best_m = d, np.full(len(p), m)
        else:
            closer = d < best_d
            best_d, best_m = np.where(closer, d, best_d), np.where(closer, m, best_m)
    return best_d, best_m


# --- filament_flow_ratio: a top surface's lines, too thin, right, too thick -----------------------

_FLOW = {"low": (0.072, 0.052), "right": (0.112, 0.064), "high": (0.150, 0.092)}


def _flow(kind):
    r, h = _FLOW[kind]
    top = 0.2
    lines = [((-0.66, y), (0.66, y), r, h, top - 0.02, M_ACCENT) for y in np.arange(-0.66, 0.67, 0.22)]

    def scene(p):
        slab = sd_box(p, (0.0, 0.0, top / 2.0), (0.85, 0.85, top / 2.0), 0.02)
        return union(bed(p), (slab, M_PART2), _beads(p, lines))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.2), extent=2.4, az=62.0, el=34.0)
    return Overlay(render(scene, cam, step=0.6, steps=220), cam).finish(SIZE)


def flow_ratio_low(k, n):
    return _flow("low")


def flow_ratio_right(k, n):
    return _flow("right")


def flow_ratio_high(k, n):
    return _flow("high")


# --- filament_diameter: a cut piece of 1.75 mm and of 2.85 mm filament, to scale ------------------


def _diameter(d_mm):
    radius = 0.165 * d_mm / 1.75

    def scene(p):
        q = np.stack([p[:, 1], p[:, 2], p[:, 0]], axis=1)  # the strand lies along x
        dyz = np.hypot(q[:, 0], q[:, 1] - radius) - radius
        dx = np.abs(q[:, 2]) - 0.8
        d = np.minimum(np.maximum(dyz, dx), 0.0) + np.hypot(np.maximum(dyz, 0.0), np.maximum(dx, 0.0))
        return union(bed(p), (d, M_ACCENT))

    cam = Camera(SIZE, target=(0.25, 0.0, 0.3), extent=2.0, az=70.0, el=22.0)
    o = Overlay(render(scene, cam), cam)
    # The diameter across the cut end.
    x = 0.8 + 0.06
    o.line((x, 0.0, 0.0), (x, 0.0, 2.0 * radius), CURSOR + (230,), 1.6)
    for z in (0.0, 2.0 * radius):
        o.line((x, -0.07, z), (x, 0.07, z), CURSOR + (230,), 1.6)
    return o.finish(SIZE)


def diameter_175(k, n):
    return _diameter(1.75)


def diameter_285(k, n):
    return _diameter(2.85)


# --- nozzle_temperature_range_low/high: a thermometer, the recommended range beside it -----------


def temperature_range(k, n):
    f = Flat(SIZE)
    cx, top, bottom = 0.40, 0.10, 0.74
    lo, hi, now = 0.56, 0.30, 0.40  # heights on the tube of the range's ends and of the print temperature
    f.rect(cx - 0.045, top, cx + 0.045, bottom, C_GRID, r=0.045)
    f.ellipse(cx, bottom + 0.05, 0.085, 0.085, C_GRID)
    f.rect(cx - 0.022, now, cx + 0.022, bottom + 0.02, C_PART, r=0.02)
    f.ellipse(cx, bottom + 0.05, 0.06, 0.06, C_PART)
    # Ticks along the tube.
    for i in range(11):
        y = top + 0.04 + (bottom - top - 0.08) * i / 10.0
        f.line([(cx - 0.11, y), (cx - 0.07 if i % 5 else cx - 0.13, y)], C_AXIS, 1.3)
    # The range: a bracket beside the tube and a band across it.
    f.rect(cx - 0.06, hi, cx + 0.06, lo, C_ACCENT + (90,))
    f.line([(cx + 0.10, hi), (cx + 0.16, hi), (cx + 0.16, lo), (cx + 0.10, lo)], C_ACCENT_LIGHT, 3.0)
    # The print temperature, inside it.
    f.poly([(cx + 0.30, now - 0.035), (cx + 0.30, now + 0.035), (cx + 0.23, now)], CURSOR)
    f.line([(cx + 0.06, now), (cx + 0.23, now)], CURSOR + (160,), 1.2)
    return f.finish()


# --- the first layer and the layers above it, for the nozzle and bed temperatures -----------------

_LAYER_H, _LAYERS = 0.11, 8


def _layers(first):
    def scene(p):
        best = None
        for i in range(_LAYERS):
            z0 = i * _LAYER_H
            d = sd_box(p, (0.0, 0.0, z0 + _LAYER_H / 2.0), (0.62, 0.62, _LAYER_H / 2.0), 0.035)
            m = M_ACCENT if (i == 0) == first else M_PART
            best = (d, np.full(len(p), m)) if best is None else (np.where(d < best[0], d, best[0]), np.where(d < best[0], m, best[1]))
        return union(bed(p), best)

    cam = Camera(SIZE, target=(0.0, 0.0, 0.42), extent=2.3)
    return Overlay(render(scene, cam, step=0.7, steps=200), cam).finish(SIZE)


def layers_first(k, n):
    return _layers(True)


def layers_other(k, n):
    return _layers(False)


# --- chamber temperature: the chamber heating up, the print starting at the minimal temperature ---


def chamber_heatup(k, n):
    f = Flat(SIZE)
    _chart_axes(f)
    ambient, target, minimal = 0.12, 0.80, 0.62
    tau = 0.28
    temp = lambda t: ambient + (target - ambient) * (1.0 - math.exp(-t / tau))
    start = -tau * math.log(1.0 - (minimal - ambient) / (target - ambient))  # where it reaches the minimal
    ts = np.linspace(0.0, 1.0, 80)
    pts = [(X(t), Y(temp(t))) for t in ts]
    # From the start on, the print: shaded under the curve.
    after = [(X(t), Y(temp(t))) for t in ts if t >= start]
    f.poly([(X(start), Y(0.0))] + [(X(start), Y(minimal))] + after + [(X(1.0), Y(0.0))], C_ACCENT + (70,))
    f.dashed((X(0.0), Y(target)), (X(1.0), Y(target)), C_PART, 1.3)
    f.dashed((X(0.0), Y(minimal)), (X(start), Y(minimal)), C_ACCENT_LIGHT, 1.6)
    f.dashed((X(start), Y(0.0)), (X(start), Y(minimal)), C_ACCENT_LIGHT, 1.6)
    f.line([q for q, t in zip(pts, ts) if t <= start], C_PART, 3.0)
    f.line(after, C_ACCENT_LIGHT, 3.2)
    f.ellipse(X(start), Y(minimal), 0.02, 0.02, C_ACCENT_LIGHT, BG, 1.5)
    return f.finish()


# --- filament_max_volumetric_speed: what one second of extrusion prints, as a thin and a thick line -


def _volumetric(thick):
    r, h = (0.15, 0.10) if thick else (0.075, 0.05)
    length = 1.7 * (0.075 * 0.05) / (r * h)
    x0 = -0.85
    line = [((x0, 0.0), (x0 + length, 0.0), r, h, 0.0, M_ACCENT)]

    def scene(p):
        return union(bed(p), _beads(p, line))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.1), extent=2.1, az=20.0, el=38.0)
    o = Overlay(render(scene, cam, step=0.6, steps=220), cam)
    # The nozzle where the second ends.
    tip = np.array([x0 + length, 0.0, 2.0 * h + 0.03])
    o.draw.polygon([o.pt(tip), o.pt(tip + (0.09, 0.0, 0.16)), o.pt(tip + (-0.09, 0.0, 0.16))], fill=C_PART2 + (255,))
    o.draw.rectangle([o.pt(tip + (-0.12, 0.0, 0.40)), o.pt(tip + (0.12, 0.0, 0.16))], fill=C_PART2 + (255,))
    # Where it started.
    o.line((x0, -0.25, 0.0), (x0, 0.25, 0.0), CURSOR + (200,), 1.4)
    return o.finish(SIZE)


def volumetric_thin(k, n):
    return _volumetric(False)


def volumetric_thick(k, n):
    return _volumetric(True)


# --- adaptive pressure advance: measured values against flow, at two accelerations, and the fit ----


def adaptive_pa(k, n):
    f = Flat(SIZE)
    _chart_axes(f)
    for a0, a1 in ((0.86, 0.46), (0.66, 0.30)):  # a slow and a fast acceleration
        curve = lambda t: a1 + (a0 - a1) * math.exp(-2.4 * t)
        ts = np.linspace(0.02, 0.98, 60)
        f.line([(X(t), Y(curve(t))) for t in ts], C_ACCENT_LIGHT, 2.8)
        for t, wobble in ((0.12, 0.02), (0.42, -0.025), (0.78, 0.015)):
            f.ellipse(X(t), Y(curve(t) + wobble), 0.022, 0.022, CURSOR, BG, 1.5)
    return f.finish()


# --- slow_down_for_layer_cooling: print speed against the time a layer would take at full speed ----
#
# GCode/CoolingBuffer.cpp: a layer quicker than slow_down_layer_time is slowed so that it takes that
# long, but not below slow_down_min_speed.


def _slow_down(floor_marked):
    f = Flat(SIZE)
    _chart_axes(f)
    T, vmax, vmin = 0.55, 0.86, 0.30
    speed = lambda t: max(vmin, vmax * t / T) if t < T else vmax
    ts = np.linspace(0.0, 1.0, 120)
    pts = [(X(t), Y(speed(t))) for t in ts]
    f.poly(pts + [(X(1.0), Y(0.0)), (X(0.0), Y(0.0))], C_ACCENT + (60,))
    f.dashed((X(T), Y(0.0)), (X(T), Y(vmax)), C_PART, 1.3)
    knee = vmin * T / vmax
    if floor_marked:
        f.line([q for q, t in zip(pts, ts) if t >= knee], C_PART, 2.6)
        f.line([(X(0.0), Y(vmin)), (X(knee), Y(vmin))], C_ACCENT_LIGHT, 4.0)
        f.dashed((X(knee), Y(vmin)), (X(1.0), Y(vmin)), C_ACCENT_LIGHT, 1.4)
    else:
        f.line(pts, C_ACCENT_LIGHT, 3.2)
    f.ellipse(X(T), Y(vmax), 0.017, 0.017, C_ACCENT_LIGHT, BG, 1.5)
    return f.finish()


def slow_down(k, n):
    return _slow_down(False)


def slow_down_min(k, n):
    return _slow_down(True)


# --- the auxiliary part cooling fan, blowing across the part from the side ------------------------


def _toolhead(f, x, y, colour=C_PART2, block=None):
    """A toolhead seen from the side, its nozzle tip at (x, y)."""
    f.rect(x - 0.11, y - 0.30, x + 0.11, y - 0.09, colour, r=0.015)
    f.rect(x - 0.06, y - 0.09, x + 0.06, y - 0.045, block or (122, 110, 108), r=0.008)
    f.poly([(x - 0.03, y - 0.045), (x + 0.03, y - 0.045), (x + 0.008, y), (x - 0.008, y)], colour)


def _fan(f, cx, cy, r, colour):
    f.rect(cx - r, cy - r, cx + r, cy + r, C_DARK, colour, 1.6, r=0.02)
    f.ellipse(cx, cy, r * 0.82, r * 0.82, BG, colour, 1.2)
    for i in range(5):
        a = 2.0 * math.pi * i / 5.0
        f.poly([(cx, cy), (cx + r * 0.75 * math.cos(a), cy + r * 0.75 * math.sin(a)),
                (cx + r * 0.75 * math.cos(a + 0.7), cy + r * 0.75 * math.sin(a + 0.7))], colour)
    f.ellipse(cx, cy, r * 0.18, r * 0.18, C_DARK)


def aux_fan(k, n):
    f = Flat(SIZE)
    bed_y = 0.80
    f.line([(0.04, bed_y), (0.96, bed_y)], C_AXIS, 2.4)
    f.rect(0.46, 0.50, 0.80, bed_y, C_PART, r=0.01)
    for y in np.arange(0.52, bed_y, 0.035):
        f.line([(0.46, y), (0.80, y)], C_PART2 + (120,), 1.0)
    _toolhead(f, 0.66, 0.49)
    _fan(f, 0.14, 0.58, 0.10, C_ACCENT_LIGHT)
    # The air: streaks from the fan across the part.
    for i, y in enumerate((0.52, 0.58, 0.64, 0.70)):
        x0 = 0.27 + 0.02 * (i % 2)
        f.arrow((x0, y), (x0 + 0.15, y), C_ACCENT_LIGHT, 2.0, 0.03)
    for y in (0.56, 0.62, 0.68, 0.74):
        f.dashed((0.82, y), (0.95, y), C_ACCENT_LIGHT + (180,), 1.4, 0.02)
    return f.finish()


# --- the exhaust fan: during the print, and once it has finished ---------------------------------


def _exhaust(printing):
    f = Flat(SIZE)
    x0, y0, x1, y1 = 0.10, 0.12, 0.90, 0.86
    f.rect(x0, y0, x1, y1, None, C_PART2, 2.4, r=0.03)
    bed_y = 0.74
    f.rect(x0 + 0.08, bed_y, x1 - 0.20, bed_y + 0.03, C_PART2)
    f.rect(0.34, 0.56 if printing else 0.50, 0.56, bed_y, C_PART, r=0.01)
    if printing:
        _toolhead(f, 0.45, 0.555)
    else:
        _toolhead(f, 0.24, 0.42)
    # The fan in the back wall, at the top, and the air leaving through it.
    _fan(f, x1 - 0.10, y0 + 0.13, 0.075, C_ACCENT_LIGHT)
    f.arrow((x1 - 0.02, y0 + 0.13), (x1 + 0.08, y0 + 0.13), C_ACCENT_LIGHT, 2.4, 0.03)
    # Fumes drifting up towards it.
    for i, (x, y) in enumerate(((0.40, 0.42), (0.52, 0.36), (0.62, 0.28), (0.70, 0.22))):
        f.arrow((x, y), (x + 0.07, y - 0.05), C_ACCENT + (200,), 1.6, 0.022)
    return f.finish()


def exhaust_during(k, n):
    return _exhaust(True)


def exhaust_after(k, n):
    return _exhaust(False)


# --- loading the new filament at a tool change (GCode/WipeTower2.cpp, toolchange_Load()) -----------
#
# From where it was parked down to the nozzle: a fifth of the way at the loading speed at the start,
# most of it fast at the loading speed, and the last tenth at a tenth of that.

_PARKED = _TUBE_TOP - 0.06


def toolchange_load(k, n):
    f = Flat(GIF_SIZE)
    _hotend(f)
    _tube_marks(f)
    span = _NOZZLE_TIP - _PARKED
    if k <= 6:
        u = 0.2 * k / 6.0
    elif k <= 9:
        u = 0.2 + 0.7 * (k - 6) / 3.0
    elif k <= 17:
        u = 0.9 + 0.1 * (k - 9) / 8.0
    else:
        u = 1.0
    tip = _PARKED + span * u
    _filament(f, tip, tip, "point" if u < 1.0 else "flat", melt=u >= 1.0)
    return f.finish()


# --- the prime tower's interface layer, where one material is printed on another ------------------


def tower_interface(k, n):
    lh, below, above = 0.09, 6, 5

    def scene(p):
        best = None
        for i in range(below + 1 + above):
            z0 = i * lh
            d = sd_box(p, (0.0, 0.0, z0 + lh / 2.0), (0.42, 0.62, lh / 2.0), 0.03)
            m = M_PART2 if i < below else (M_ACCENT if i == below else M_PART)
            best = (d, np.full(len(p), m)) if best is None else (np.where(d < best[0], d, best[0]), np.where(d < best[0], m, best[1]))
        return union(bed(p), best)

    cam = Camera(SIZE, target=(0.0, 0.0, 0.52), extent=2.1)
    return Overlay(render(scene, cam, step=0.7, steps=200), cam).finish(SIZE)


# --- idle temperature: the parked tool kept warm while another prints ------------------------------


def idle_temperature(k, n):
    f = Flat(SIZE)
    bed_y = 0.82
    f.line([(0.04, bed_y), (0.96, bed_y)], C_AXIS, 2.4)
    f.rect(0.12, 0.66, 0.50, bed_y, C_PART, r=0.01)
    _toolhead(f, 0.32, 0.655)
    # The parked one, at the side, up at its dock.
    f.rect(0.62, 0.10, 0.94, 0.14, C_DARK, r=0.01)
    _toolhead(f, 0.78, 0.46, C_PART2, C_ACCENT_LIGHT)
    for x in (0.70, 0.78, 0.86):  # warmth rising off its heater block
        f.line([(x, 0.39), (x + 0.015, 0.36), (x - 0.01, 0.33), (x + 0.01, 0.30)], C_ACCENT_LIGHT + (170,), 1.6)
    return f.finish()


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "flow_ratio_low": 0,
    "flow_ratio_right": 0,
    "flow_ratio_high": 0,
    "diameter_175": 0,
    "diameter_285": 0,
    "temperature_range": 0,
    "layers_first": 0,
    "layers_other": 0,
    "chamber_heatup": 0,
    "volumetric_thin": 0,
    "volumetric_thick": 0,
    "adaptive_pa": 0,
    "slow_down": 0,
    "slow_down_min": 0,
    "aux_fan": 0,
    "exhaust_during": 0,
    "exhaust_after": 0,
    "toolchange_load": 20,
    "tower_interface": 0,
    "idle_temperature": 0,
}
