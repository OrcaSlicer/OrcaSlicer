"""Pictures for the tooltips of the Process tab's Speed page (src/slic3r/GUI/SettingTipsProcessSpeed.cpp),
and the feature pictures that the other pages share.

The feature pictures, feature_<name>.png, show one kind of line: the settings that change one kind of
line (its speed, acceleration, jerk, width or flow) all point at the picture of that line. They are one
test part, sliced once by OrcaSlicer (settings_process_speed_slice.py), drawn in the plater's view with
its front right corner cut away so that the inside shows, and differ only in which lines are in ACCENT.

The rest are diagrams of what the Speed page's settings do, drawn with PIL in the same palette: a wall
in section for the overhang speeds, and the speed of the nozzle along a path for acceleration, jerk and
the like.
"""

import gzip
import json
import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, PART, PART2, SIZE, SS
from .settings_process import Model, Plan, _clip, _paths, _rgb
from .settings_process_speed_slice import DATA

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _layers():
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data


# --- the feature pictures -------------------------------------------------------------------------

FIRST_Z = 0.35  # the first layer is at 0.3 mm
# The front of the part is cut away as a staircase, each step down to a layer that shows one kind of
# infill from above: (x0, x1, y0, y1, z): what lies in the rectangle above z is left out. From the left:
# internal solid infill, the internal bridge over the sparse infill, the sparse infill, down to the
# bottom surface, and on the deck, the bridge over the gap to the pier.
CUTS = ((-12.0, -5.5, -12.0, 0.5, 6.65), (-5.5, -1.9, -12.0, 0.5, 6.35), (-1.9, 1.0, -2.5, 0.5, 3.05),
        (-1.9, 1.0, -12.0, -2.5, 0.35), (1.0, 12.0, -12.0, 0.5, 5.45))
VIEW = dict(target=(-0.42, -0.12, 0.3), extent=3.05, el=36.0)
TRAVEL_W = 0.16  # how wide a travel move is drawn, in mm
TRAVEL_MIN = 3.0  # mm: shorter travel moves are left out
LIFT = 0.12  # how far above its layer a travel move is drawn, so that it is not hidden in the layer's lines


def _cut(paths, z):
    """The paths, less what the cuts take away at height z."""
    for x0, x1, y0, y1, h in CUTS:
        if z <= h:
            continue
        band = _clip(_clip(paths, (-1.0, 0.0), -x0), (1.0, 0.0), x1)
        paths = (_clip(paths, (1.0, 0.0), x0) + _clip(paths, (-1.0, 0.0), -x1)
                 + _clip(band, (0.0, 1.0), y0) + _clip(band, (0.0, -1.0), -y1))
    return paths


# Feature name -> which lines it is: a test on (z, G-code type).
FEATURES = {
    "outer_wall": lambda z, t: t == "Outer wall",
    "inner_wall": lambda z, t: t == "Inner wall",
    "top_surface": lambda z, t: t == "Top surface",
    "bottom_surface": lambda z, t: t == "Bottom surface",
    "sparse_infill": lambda z, t: t == "Sparse infill",
    "internal_solid_infill": lambda z, t: t == "Internal solid infill",
    "bridge": lambda z, t: t == "Bridge",
    "internal_bridge": lambda z, t: t == "Internal Bridge",
    "gap_fill": lambda z, t: t == "Gap infill",
    "overhang": lambda z, t: t == "Overhang wall",
    "support": lambda z, t: t in ("Support", "Support transition"),
    "support_interface": lambda z, t: t == "Support interface",
    "skirt": lambda z, t: t == "Skirt",
    "brim": lambda z, t: t == "Brim",
    "first_layer": lambda z, t: z < FIRST_Z and t != "Travel",
    "travel": lambda z, t: t == "Travel",
    "ironing": lambda z, t: t == "Ironing",
    "prime_tower": lambda z, t: t == "Prime tower",
}


def _stray(p):
    """A path drawn nowhere: the travel in from the printer's home and out to it, and the extruding move
    off the prime tower that the G-code still labels as the tower."""
    pts = p["pts"]
    if p["t"] == "Travel":
        return np.abs(pts).max() > 30.0
    return p["t"] == "Prime tower" and (pts[:, 0].max() > -11.0 or pts[:, 1].min() < 0.0)


LIT = ACCENT * 1.15


def _lines(colour):
    """The test part with each line in colour(z, type): ACCENT for what the picture is about, or None to
    leave the line out. Travel and ironing are only drawn where a picture asks for them: ironing goes over
    the top surface again and would hide it, and travel would cover everything."""
    def picture(k, n):
        items = []
        for layer in _layers():
            z = layer["z"]
            for p in _cut([p for p in _paths(layer, lambda t: True) if not _stray(p)], z):
                c = colour(z, p["t"])
                if p["t"] == "Travel":
                    # The moves between islands; the short hops inside one would only add noise.
                    if c is not None and np.linalg.norm(np.diff(p["pts"], axis=0), axis=1).sum() > TRAVEL_MIN:
                        items.append((z + LIFT, TRAVEL_W, dict(p, w=TRAVEL_W), ACCENT * 1.25))
                elif p["t"] == "Ironing":
                    if c is not None:
                        items.append((z + 0.05, 0.2, p, c))
                else:
                    items.append((z, 0.2, p, PART if c is None else c))
        return Model(items, SIZE, **VIEW).finish()
    return picture


def _feature(test):
    return _lines(lambda z, t: LIT if test(z, t) else None)


# More pictures of the same part, for the Speed page's settings that cover a group of lines.
FIRST_WALLS = ("Outer wall", "Inner wall", "Overhang wall", "Brim")
GROUPS = {
    # initial_layer_speed: the first layer's walls and brim; initial_layer_infill_speed: the rest of it.
    "first_layer_walls": lambda z, t: z < FIRST_Z and t in FIRST_WALLS,
    "first_layer_infill": lambda z, t: z < FIRST_Z and t not in FIRST_WALLS and t != "Travel",
    "first_layer_travel": lambda z, t: z < FIRST_Z and t == "Travel",
    # bridge_acceleration: everything printed over nothing.
    "bridges": lambda z, t: t in ("Bridge", "Internal Bridge", "Overhang wall"),
    # infill_jerk: every kind of infill except the top surface, which has a jerk of its own.
    "infill": lambda z, t: t in ("Sparse infill", "Internal solid infill", "Bridge", "Internal Bridge",
                                 "Bottom surface"),
}

SLOW_LAYERS = 7  # slow_down_layers: the speed comes back over the first layers (more than usual, to be seen)


def _slow_layer(z, t):
    """The first layers in ACCENT fading to grey, the way their speed rises back to normal."""
    if t in ("Travel", "Ironing"):
        return None
    f = min(max((z / 0.3 - 1.0) / SLOW_LAYERS, 0.0), 1.0)
    return LIT * (1.0 - f ** 1.5) + PART * f ** 1.5 if f < 1.0 else None


FRAMES = {}


def _add(name, frames, fn):
    FRAMES[name] = frames
    globals()[name] = fn


for _name, _test in FEATURES.items():
    _add("feature_" + _name, 0, _feature(_test))
for _name, _test in GROUPS.items():
    _add("speed_" + _name, 0, _feature(_test))
_add("speed_slow_down_layers", 0, _lines(_slow_layer))


# --- diagrams: the nozzle's speed, overhangs in section, small loops from above -------------------

BG = _rgb(BED, 0.75)
DIM = _rgb(PART2, 0.8)  # axes, guides, the other value's curve
GREY = _rgb(PART)
LINE = _rgb(ACCENT, 1.25)
FILL = _rgb(ACCENT, 0.45)


class Canvas:
    """A picture drawn in PIL at four times its size, with (0, 0) bottom left and (1, 1) top right of the
    box (x0, y0, x1, y1) given in fractions of the picture."""

    def __init__(self, size=SIZE, box=(0.12, 0.14, 0.88, 0.86)):
        self.size = size
        self.px = size * SS * 2
        self.box = box
        self.image = Image.new("RGB", (self.px, self.px), BG)
        self.draw = ImageDraw.Draw(self.image)

    def xy(self, x, y):
        x0, y0, x1, y1 = self.box
        return ((x0 + (x1 - x0) * x) * self.px, (1.0 - (y0 + (y1 - y0) * y)) * self.px)

    def w(self, width):
        """A line width given in pixels of the finished picture."""
        return max(1, int(round(width * SS * 2)))

    def line(self, pts, colour, width=2.0):
        xy = [self.xy(x, y) for x, y in pts]
        self.draw.line(xy, fill=colour, width=self.w(width), joint="curve")
        r = self.w(width) / 2.0
        for x, y in (xy[0], xy[-1]):
            self.draw.ellipse([x - r, y - r, x + r, y + r], fill=colour)

    def dashed(self, pts, colour, width=1.2, dash=0.025):
        pts = np.asarray(pts, float)
        seg = np.linalg.norm(np.diff(pts, axis=0), axis=1)
        cum = np.concatenate([[0.0], np.cumsum(seg)])
        for a in np.arange(0.0, cum[-1], 2 * dash):
            b = min(a + dash, cum[-1])
            ts = np.linspace(a, b, 4)
            self.line([tuple(np.array([np.interp(t, cum, pts[:, 0]), np.interp(t, cum, pts[:, 1])])) for t in ts],
                      colour, width)

    def area(self, pts, colour):
        """The area between the curve pts and y = 0."""
        poly = [self.xy(pts[0][0], 0.0)] + [self.xy(x, y) for x, y in pts] + [self.xy(pts[-1][0], 0.0)]
        self.draw.polygon(poly, fill=colour)

    def dot(self, x, y, radius, colour, outline=None):
        px, py = self.xy(x, y)
        r = radius * SS * 2
        self.draw.ellipse([px - r, py - r, px + r, py + r], fill=colour, outline=outline, width=self.w(1.0) if outline else 0)

    def axes(self):
        self.line([(0.0, 1.04), (0.0, 0.0), (1.04, 0.0)], DIM, 1.4)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _profile(segments, a, scale):
    """The speed over time of moves one after another: segments are (length, top speed, speed at its end);
    each starts at the speed the one before ended at, speeds up at acceleration a as far as it can, and
    slows down to its end speed in time. The first starts from standing. Returns [(t, v)] with t divided
    by `scale`."""
    out, t, v0 = [], 0.0, 0.0
    for d, top, v1 in segments:
        # The highest speed this move can reach and still slow down to v1 by its end.
        peak = min(top, math.sqrt(max(a * d + (v0 * v0 + v1 * v1) / 2.0, 0.0)))
        t_up, t_down = (peak - v0) / a, (peak - v1) / a
        cruise = max(d - (peak * peak - v0 * v0) / (2 * a) - (peak * peak - v1 * v1) / (2 * a), 0.0) / peak
        out += [(t, v0), (t + t_up, peak), (t + t_up + cruise, peak), (t + t_up + cruise + t_down, v1)]
        t += t_up + cruise + t_down
        v0 = v1
    return [(x / scale, y) for x, y in out]


def _speed_graph(curve, ghost, corners=None, path=None):
    """A speed graph: the setting's curve in ACCENT over the area under it, the other value's in dim grey.
    Over it, the path the moves follow, with its corners where the speed dips."""
    c = Canvas(box=(0.12, 0.12, 0.88, 0.62) if path else (0.12, 0.14, 0.88, 0.86))
    c.axes()
    if ghost:
        c.dashed(ghost, DIM, 1.3)
    c.area(curve, FILL)
    c.line(curve, LINE, 2.4)
    if path:
        top = Canvas(box=(0.12, 0.70, 0.88, 0.92))
        top.image, top.draw = c.image, c.draw
        top.line(path, GREY, 2.2)
        for x, y in corners or []:
            top.dot(x, y, 3.2, LINE)
    return c.finish()


def _accel(a):
    """One move, from standing to standing: gentle acceleration takes longer to reach the top speed."""
    def picture(k, n):
        moves = [(1.0, 0.85, 0.0)]
        other = 9.0 if a < 3.0 else 1.6
        return _speed_graph(_profile(moves, a, 1.6), _profile(moves, other, 1.6),
                            path=[(0.0, 0.5), (1.0, 0.5)], corners=[(0.0, 0.5), (1.0, 0.5)])
    return picture


def _fit(curve, ghost):
    """Both curves scaled in time so that the longer one fills the graph."""
    span = max(curve[-1][0], ghost[-1][0])
    return [(x / span, y) for x, y in curve], [(x / span, y) for x, y in ghost]


def _zigzag(curve):
    """The path of the moves over the graph: a corner where each move ends."""
    ends = [curve[0][0]] + [x for i, (x, y) in enumerate(curve) if i % 4 == 3]
    return [(t, 0.15 if i % 2 == 0 else 0.85) for i, t in enumerate(ends)]


def _corners(vj):
    """Moves along a zigzag: at each corner the nozzle slows down to the jerk's (or junction deviation's)
    speed, so a low value nearly stops at every corner."""
    def picture(k, n):
        other = 0.6 if vj < 0.3 else 0.08
        curve, ghost = _fit(*(_profile([(0.5, 0.85, v)] * 3 + [(0.5, 0.85, 0.0)], 4.0, 1.0) for v in (vj, other)))
        zig = _zigzag(curve)
        return _speed_graph(curve, ghost, path=zig, corners=zig[1:-1])
    return picture


def _cruise(peak):
    """Short moves: Klipper's accel_to_decel (minimum cruise ratio) caps the speed they reach, so a low
    value keeps the nozzle from shooting up to speed and straight back down on each."""
    def picture(k, n):
        other = 0.85 if peak < 0.6 else 0.45
        curve, ghost = _fit(*(_profile([(0.22, p, 0.0)] * 5, 5.0, 1.0) for p in (peak, other)))
        zig = _zigzag(curve)
        return _speed_graph(curve, ghost, path=zig, corners=zig)
    return picture


def _flow_step(slope=None, steps=None):
    """The flow over time where a slow feature meets a fast one: asked for, it jumps (dim); with extrusion
    rate smoothing it ramps (ACCENT), more gently the lower the slope, in steps as long as the segments."""
    hi, lo = 0.85, 0.3

    def asked(x):
        return hi if x < 0.3 or x > 0.7 else lo

    def picture(k, n):
        ramp = 0.18 if slope == "low" else 0.05
        if steps is not None:
            ramp = 0.16

        def smooth_flow(x):
            if x < 0.3:
                return hi if x < 0.3 - ramp else hi - (hi - lo) * (x - (0.3 - ramp)) / ramp
            if x < 0.7:
                return lo
            return min(hi, lo + (hi - lo) * (x - 0.7) / ramp)

        xs = np.linspace(0.0, 1.0, 400)
        if steps is not None:
            q = 1.0 / (40 if steps == "short" else 9) * 2 * ramp
            xs2 = np.floor(xs / q) * q
            curve = [(x, smooth_flow(x2 + q)) for x, x2 in zip(xs, xs2)]
        else:
            curve = [(x, smooth_flow(x)) for x in xs]
        ghost = [(0.0, hi), (0.3, hi), (0.3, lo), (0.7, lo), (0.7, hi), (1.0, hi)]
        return _speed_graph(curve, ghost)
    return picture


# --- overhangs in section ----------------------------------------------------------------------
#
# A wall two lines thick, seen end on: four layers straight up, then each layer hangs further out over
# the one below - by 10-25%, 25-50%, 50-75% and 75-100% of a line's width, the four overhang speeds. Beside
# each layer, a bar as long as its speed: the further a line hangs out, the slower it is printed.

BEAD_W, BEAD_H = 1.0, 0.45  # a line's section, in units of its width
OVERHANG = (0.0, 0.0, 0.0, 0.0, 0.18, 0.37, 0.62, 0.87)  # how far each layer hangs over the one below
OVERHANG_SPEED = (1.0, 1.0, 1.0, 1.0, 0.8, 0.6, 0.42, 0.28)


def _bead(c, x, y, colour, tilt=0.0):
    """A line's section, rounded at both ends, its bottom left corner at (x, y), turned up by `tilt` radians
    about its left end."""
    pts = []
    r = BEAD_H / 2.0
    for cx, a0 in ((x + BEAD_W - r, -90.0), (x + r, 90.0)):
        for i in range(13):
            a = math.radians(a0 + 180.0 * i / 12)
            pts.append((cx + r * math.cos(a), y + r + r * math.sin(a)))
    ca, sa = math.cos(tilt), math.sin(tilt)
    pts = [(x + (px - x) * ca - (py - y) * sa, y + (px - x) * sa + (py - y) * ca) for px, py in pts]
    c.draw.polygon([c.xy(*p) for p in pts], fill=colour, outline=_rgb(BED, 0.5), width=c.w(1.0))


class Section(Canvas):
    """A Canvas in units of a line's width: (0, 0) at the bottom of the wall's outer face."""

    LEFT, SPAN = -3.75, 7.0

    def xy(self, x, y):
        s = self.px / self.SPAN
        return ((x - self.LEFT) * s, self.px * 0.80 - y * s)


BAR_X, BAR_LEN = -3.5, 1.9  # the speed bars, left of the wall


def _overhang(band=None, curled=False):
    """band None: every overhanging line in ACCENT, deeper the further it hangs; band 1-4: that one alone."""
    def picture(k, n):
        c = Section()
        x = 0.0
        rows = len(OVERHANG) - (1 if curled else 0)
        for i in range(rows):
            x += OVERHANG[i] * BEAD_W
            y = i * BEAD_H
            b = i - 3  # the overhang band of this layer, 1-4
            if band is None:
                colour = _rgb(PART) if b < 1 else _rgb(PART * (1 - b / 4.0) + ACCENT * 1.25 * (b / 4.0))
            else:
                colour = LINE if b == band else GREY
            tilt = math.radians(14.0) if curled and i == rows - 1 else 0.0
            if curled:
                colour = GREY
            _bead(c, x - BEAD_W, y, _rgb(PART, 0.8))  # the inner wall
            _bead(c, x, y, colour, tilt)
            bar = OVERHANG_SPEED[i]
            c.line([(BAR_X, y + BEAD_H / 2), (BAR_X + BAR_LEN * bar, y + BEAD_H / 2)],
                   colour if (band is None or b == band) and b >= 1 and not curled else DIM, 3.0)
        if curled:
            # The line printed over the curled-up one, slowed down so the nozzle does not catch on it.
            y = rows * BEAD_H + 0.12
            _bead(c, x + 0.25, y, LINE)
            _bead(c, x + 0.25 - BEAD_W, y, _rgb(PART, 0.8))
            c.line([(BAR_X, y + BEAD_H / 2), (BAR_X + BAR_LEN * 0.3, y + BEAD_H / 2)], LINE, 3.0)
        # The outer face of the straight wall, carried on up as a guide to how far the overhang reaches.
        c.dashed([(BEAD_W, 0.0), (BEAD_W, len(OVERHANG) * BEAD_H + 0.2)], DIM, 1.0, dash=0.06)
        return c.finish()
    return picture


# --- small loops from above ----------------------------------------------------------------------


def _ring(r, cx=0.0, cy=0.0, w=0.45):
    a = np.linspace(0.0, 2 * math.pi, max(24, int(r * 24)))
    return {"pts": np.stack([cx + r * np.cos(a), cy + r * np.sin(a)], axis=1), "w": w}


def _rounded(half_x, half_y, r, inset=0.0, w=0.45):
    hx, hy, rr = half_x - inset, half_y - inset, r - inset
    pts = []
    for cx, cy, a0 in ((hx - rr, hy - rr, 0), (rr - hx, hy - rr, 90), (rr - hx, rr - hy, 180), (hx - rr, rr - hy, 270)):
        for i in range(9):
            a = math.radians(a0 + 90.0 * i / 8)
            pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    pts.append(pts[0])
    return {"pts": np.asarray(pts), "w": w}


HOLES = ((-5.2, 1.0, 0.9), (-1.4, -1.6, 1.6), (3.8, 0.6, 2.9))  # x, y, radius in mm


def _small_loops(limit, support=False):
    """A plate with three holes from above (support: tree branches in section). The loops no longer than a
    circle of radius `limit` are the small ones, in ACCENT."""
    def picture(k, n):
        plan = Plan(18.0, SIZE)
        if support:
            plan.beads([_rounded(8.6, 5.6, 1.5)], _rgb(PART, 0.45), edge=None)  # the part above, faint
            for x, y, r in ((-5.6, 1.6, 0.7), (-2.6, -2.2, 1.1), (1.4, 1.8, 1.7), (5.4, -1.4, 2.4)):
                for rr in np.arange(r, 0.2, -0.5):
                    plan.beads([_ring(rr, x, y, 0.4)], LINE if r <= limit else _rgb(PART, 1.15))
            return plan.finish()
        plan.beads([_rounded(8.4, 5.4, 1.5), _rounded(8.4, 5.4, 1.5, 0.45)], GREY)
        for x, y, r in HOLES:
            colour = LINE if r <= limit else GREY
            plan.beads([_ring(r + 0.225, x, y), _ring(r + 0.675, x, y)], colour)
        return plan.finish()
    return picture


_add("speed_accel_low", 0, _accel(1.6))
_add("speed_accel_high", 0, _accel(9.0))
_add("speed_jerk_low", 0, _corners(0.08))
_add("speed_jerk_high", 0, _corners(0.6))
_add("speed_accel_to_decel_low", 0, _cruise(0.45))
_add("speed_accel_to_decel_high", 0, _cruise(0.85))
_add("speed_ers_low", 0, _flow_step(slope="low"))
_add("speed_ers_high", 0, _flow_step(slope="high"))
_add("speed_ers_segment_low", 0, _flow_step(steps="short"))
_add("speed_ers_segment_high", 0, _flow_step(steps="long"))
_add("speed_overhang", 0, _overhang())
for _b in range(1, 5):
    _add("speed_overhang_%d" % _b, 0, _overhang(band=_b))
_add("speed_curled_perimeters", 0, _overhang(curled=True))
_add("speed_small_perimeter_low", 0, _small_loops(1.0))
_add("speed_small_perimeter_high", 0, _small_loops(2.0))
_add("speed_small_support_low", 0, _small_loops(0.8, support=True))
_add("speed_small_support_high", 0, _small_loops(1.8, support=True))
