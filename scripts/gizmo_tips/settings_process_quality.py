"""Pictures for the tooltips of the Process tab's Quality page (src/slic3r/GUI/SettingTipsProcessQuality.cpp).

Drawn the way settings_process.py draws the earlier Process pictures, with its plans and models, from
toolpaths that settings_process_quality_slice.py sliced: a setting's values share one model and one
framing, so whatever differs between two pictures is the setting. The setting's lines are in ACCENT.

A few are diagrams instead, where no sliced model shows the point at a readable size: the scarf seam,
whose ramp is a fraction of a layer high, drawn as a section through the wall unrolled flat; the beads
of a surface at too little or too much flow, in section; and the curve of the small-area flow model.
"""

import gzip
import json
import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, GIF_SIZE, PART, PART2, SIZE, SS, phase, smooth
from .settings_process import EDGE, LINE, UNDER, WALL, Model, Plan, _layer, _partial, _paths, _rgb
from .settings_process_quality_slice import DATA

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case, value):
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data[case][value]


MOVE = (205, 214, 214)  # travels: a thin light line
ACCENT_BEAD = ACCENT * 1.15
DIM_ACCENT = _rgb(ACCENT, 0.75)


def _is(*types):
    return lambda t: t in types


def _walls(t):
    return "wall" in t


def _not_moves(t):
    return t not in ("Travel", "Wipe")


def _radius(p):
    return float(np.hypot(p["pts"][:, 0], p["pts"][:, 1]).max())


def _items(layers, test, colour):
    """Beads for Model: each path of each layer, at the layer's height."""
    return [(layer["z"], layer["h"] or 0.2, p, colour(layer, p) if callable(colour) else colour)
            for layer in layers for p in _paths(layer, test)]


def _items_z(layers, test, colour):
    """Beads that follow each point's own height (a scarf's ramp, Z contouring): the paths cut into runs
    of two points, each at its own height. colour(layer, path, z) picks a run's colour."""
    items = []
    for layer in layers:
        h = layer["h"] or 0.2
        for p in layer["paths"]:
            if not test(p["t"]):
                continue
            pts = np.asarray(p["p"], float).reshape(-1, 2)
            zs = np.asarray(p["z"], float)
            for i in range(len(pts) - 1):
                z = 0.5 * (zs[i] + zs[i + 1])
                items.append((z, h, dict(p, pts=pts[i:i + 2]), colour(layer, p, z)))
    return items


def _fixed(name, fn):
    FRAMES[name] = 0
    globals()[name] = lambda k, n: fn()


FRAMES = {}


# --- layer height, first layer height: the side of a half sphere, the foot of a block ------------


def _bead(sec, x, z, w, h, x0, z0, span, fill):
    """A bead in section: w wide and h high, its top at z, drawn into `sec` whose corner is (x0, z0) mm."""
    (l, t), (r, b) = sec.xy((x - w / 2 - x0) / span, (z - z0) / span), sec.xy((x + w / 2 - x0) / span, (z - h - z0) / span)
    sec.draw.rounded_rectangle([l, t, r, b], radius=min(r - l, b - t) * 0.48, fill=fill, outline=EDGE, width=SS * 2)


def _layer_height(value):
    """The right half of a half sphere, cut through its middle: each layer a slab out to its outer wall,
    over the sphere's own outline."""
    sec = Section()
    span = 9.0
    for layer in _case("layer_height", value):
        h = layer["h"]
        walls = _paths(layer, _is("Outer wall", "Overhang wall"))
        if not walls:
            continue
        r = max(_radius(p) + p["w"] / 2 for p in walls)
        _bead(sec, (r - 0.3) / 2, layer["z"], r + 0.3, h, -0.3, -0.25, span, LINE)
    sec.draw.line([sec.xy((8.0 * math.cos(a) + 0.3) / span, (8.0 * math.sin(a) + 0.25) / span)
                   for a in np.linspace(0, math.pi / 2, 90)], fill=MOVE, width=SS * 2)
    return sec.finish()


def _edge(layers, colour, span=2.4):
    """A block's walls where they meet its right side, in section: one bead per wall and layer, the model's
    side as a thin line."""
    sec = Section()
    x0, z0 = 8.0 - span + 0.5, -0.25
    for layer in layers:
        for p in _paths(layer, _walls):
            x = float(p["pts"][:, 0].max())
            _bead(sec, x, layer["z"], p["w"], layer["h"], x0, z0, span, colour(layer))
    sec.draw.line([sec.xy((8.0 - x0) / span, 0.0), sec.xy((8.0 - x0) / span, 1.0)], fill=MOVE, width=SS * 2)
    return sec.finish()


def _first_layer(value):
    layers = _case("initial_layer_print_height", value)
    first = min(layer["z"] for layer in layers)
    return _edge(layers, lambda layer: LINE if layer["z"] == first else WALL)


# --- line width: one layer of a rounded block -----------------------------------------------------


def _line_width(value):
    layer = _layer(_case("line_width", value), 1.6)
    plan = Plan(18.5, SIZE)
    plan.beads(_paths(layer, lambda t: not _walls(t)), DIM_ACCENT)
    plan.beads(_paths(layer, _walls), LINE)
    return plan.finish()


# --- staggered inner seams, seam gap: the back of a cylinder's walls, from above -----------------


def _staggered(value):
    layer = _case("staggered_inner_seams", value)[0]
    walls = _paths(layer, _walls)
    plan = Plan(6.0, SIZE, centre=(0.0, 5.4))
    plan.beads(walls, WALL)
    for p in walls:
        plan.dot(p["pts"][0], 4.2, _rgb(ACCENT, 1.4))
    return plan.finish()


def _seam_gap(value):
    layer = _case("seam_gap", value)[0]
    outer = _paths(layer, _is("Outer wall"))
    plan = Plan(3.2, SIZE, centre=outer[0]["pts"][0] + np.array([0.0, -0.45]))
    plan.beads(_paths(layer, _is("Inner wall")), WALL, thin=0.5)
    plan.beads(outer, LINE, thin=0.5)
    return plan.finish()


# --- scarf joint seam: a tube; the ramp each loop starts on ---------------------------------------


def _near(pts, line, d):
    """Which of `pts` lie within d of the polyline `line`."""
    a, b = line[:-1], line[1:]
    e = b - a
    t = np.clip(((pts[:, None, :] - a[None]) * e[None]).sum(-1) / np.maximum((e * e).sum(-1), 1e-12)[None], 0.0, 1.0)
    q = a[None] + e[None] * t[..., None]
    return (np.linalg.norm(pts[:, None, :] - q, axis=-1) < d).any(axis=1)


def _scarf(value):
    """The tube's walls, with the stretch each loop's scarf covers in ACCENT: the ramp the loop starts on,
    and the end of the loop that passes back over it at full height."""
    layers = _case("seam_slope_type", value)
    items = []
    for layer in layers:
        h = layer["h"] or 0.2
        paths = [dict(p, pts=np.asarray(p["p"], float).reshape(-1, 2), zs=np.asarray(p["z"], float))
                 for p in layer["paths"] if _walls(p["t"])]
        ramps = [p["pts"][p["zs"] < layer["z"] - 0.004] for p in paths]
        ramps = [r for r in ramps if len(r) > 1]
        for p in paths:
            mid = 0.5 * (p["pts"][:-1] + p["pts"][1:])
            on = np.zeros(len(mid), bool)
            for r in ramps:
                on |= _near(mid, r, 0.2)
            for i in range(len(mid)):
                z = 0.5 * (p["zs"][i] + p["zs"][i + 1])
                items.append((z, h, dict(p, pts=p["pts"][i:i + 2]), ACCENT_BEAD if on[i] else PART))
    model = Model(items, SIZE, target=(0.0, 0.0, 0.25), extent=2.0, el=40.0)
    if value == "none":  # no ramp: the seam is where each outer loop starts and ends
        for layer in layers:
            for p in _paths(layer, _is("Outer wall")):
                x, y = p["pts"][0]
                if math.hypot(x, y) > 6.0:
                    model.dot(x, y, layer["z"] - 0.1, 2.0, _rgb(ACCENT, 1.3))
    return model.finish()


# --- the scarf in section: the wall around the seam, unrolled flat --------------------------------
#
# Three layers of one wall, the seam in the middle third, layers drawn far taller than they are. Each
# loop starts on a ramp rising from the start height to the full layer (ACCENT), runs round, and ends by
# passing over that ramp again at full height with its flow running out (dimmer ACCENT), which fills
# the wedge above the ramp.


class Section:
    """A side view in 2D, 0..1 across and 0..1 up, drawn at four times the size."""

    def __init__(self, size=SIZE):
        self.size = size
        self.px = size * SS * 2
        self.image = Image.new("RGB", (self.px, self.px), _rgb(BED, 0.75))
        self.draw = ImageDraw.Draw(self.image)

    def xy(self, x, y):
        return (x * self.px, (1.0 - y) * self.px)

    def poly(self, pts, fill, outline=EDGE, width=1.0):
        self.draw.polygon([self.xy(x, y) for x, y in pts], fill=fill, outline=outline, width=int(width * SS * 2))

    def ellipse(self, cx, cy, rx, ry, fill, outline=EDGE):
        (x0, y0), (x1, y1) = self.xy(cx - rx, cy + ry), self.xy(cx + rx, cy - ry)
        self.draw.ellipse([x0, y0, x1, y1], fill=fill, outline=outline, width=SS * 2)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _scarf_section(start=0.0, length=0.45, steps=10, entire=False):
    s = Section()
    x0, x1 = 0.06, 0.94  # the wall shown
    h, y0 = 0.17, 0.24  # layer height, top of the layers below
    seam = x0 if entire else 0.5 - 0.5 * length
    end = x1 if entire else seam + length
    s.poly([(x0, 0.06), (x1, 0.06), (x1, y0), (x0, y0)], _rgb(PART2, 0.8))
    for i in range(3):
        b = y0 + i * h
        rise = [(seam + (end - seam) * k / steps, b + h * (start + (1.0 - start) * k / steps)) for k in range(steps + 1)]
        ramp = [(seam, b)]
        for k in range(steps):  # each segment of the ramp is printed at one height: a staircase
            ramp += [(rise[k][0], rise[k + 1][1]), (rise[k + 1][0], rise[k + 1][1])]
        ramp += [(end, b)]
        if not entire:
            s.poly([(x0, b), (seam, b), (seam, b + h), (x0, b + h)], WALL)
            s.poly([(end, b), (x1, b), (x1, b + h), (end, b + h)], WALL)
        s.poly([(seam, b + h)] + ramp[1:-1] + [(end, b + h)], DIM_ACCENT)
        s.poly(ramp, LINE)
    return s.finish()


# --- resolution and arc fitting: how many moves a line takes ---------------------------------------


def _move_ends(plan, p, radius, colour):
    for i in p.get("k", []):
        plan.dot(p["pts"][i], radius, colour, outline=None)


def _resolution(value):
    layer = _case("resolution", value)[0]
    plan = Plan(6.0, SIZE, centre=(3.3, 3.3))
    plan.beads(_paths(layer, _is("Inner wall")), WALL)
    outer = _paths(layer, _is("Outer wall"))
    plan.beads(outer, LINE)
    for p in outer:
        _move_ends(plan, p, 3.0, (235, 242, 242))
    return plan.finish()


def _arcs(value):
    layer = _case("enable_arc_fitting", value)[0]
    plan = Plan(9.0, SIZE, centre=(3.6, 3.6))
    paths = _paths(layer, _not_moves)
    plan.beads(paths, _rgb(PART, 0.8), thin=0.7)
    for p in paths:
        _move_ends(plan, p, 2.6, _rgb(ACCENT, 1.45))
    return plan.finish()


# --- X-Y compensation: a plate with a hole, against the model's outline ---------------------------


def _xy(case, value):
    layer = _case(case, value)[0]
    plan = Plan(9.5, SIZE, centre=(-3.0, 3.0))
    hole = lambda p: _radius(p) < 5.0  # noqa: E731
    walls = _paths(layer, _walls)
    plan.beads(_paths(layer, lambda t: not _walls(t)), _rgb(PART, 0.6))
    mine = [p for p in walls if hole(p) == (case == "xy_hole_compensation") and p["t"] == "Outer wall"]
    plan.beads([p for p in walls if not any(p is q for q in mine)], WALL)
    plan.beads(mine, LINE)
    # The model's edges, thin and light.
    ring = [(3.0 * math.cos(a), 3.0 * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 120)]
    square = [(-7, -7), (7, -7), (7, 7), (-7, 7), (-7, -7)]
    for line in (ring, square):
        plan.draw.line(plan.xy(line), fill=MOVE, width=SS * 2)
    return plan.finish()


# --- elephant foot: the foot of a block's corner, from low down -----------------------------------


def _elephant(value):
    layers = _case("elefant_foot_compensation", value)
    zs = sorted(layer["z"] for layer in layers)[:{"off": 0, "on": 1, "layers": 5}[value]]
    return _edge(layers, lambda layer: LINE if layer["z"] in zs else WALL)


# --- polyholes: a small hole from above, the layers beneath fading ---------------------------------


def _polyhole(value):
    layers = _case("hole_to_polyhole", value)
    plan = Plan(4.8, SIZE)
    hole = lambda layer: [p for p in _paths(layer, _walls) if _radius(p) < 2.6]  # noqa: E731
    for j, layer in enumerate(layers[-5:-1]):
        plan.beads(hole(layer), _rgb(UNDER, 0.45 + 0.05 * j), edge=None)
    top = hole(layers[-1])
    plan.beads([p for p in top if p["t"] != "Outer wall"], WALL)
    plan.beads([p for p in top if p["t"] == "Outer wall"], LINE)
    plan.draw.line(plan.xy([(1.6 * math.cos(a), 1.6 * math.sin(a)) for a in np.linspace(0, 2 * math.pi, 90)]),
                   fill=MOVE, width=SS * 2)
    return plan.finish()


# --- ironing: a corner of an ironed top, close up --------------------------------------------------


def _ironing(case, value):
    layer = _case(case, value)[0]
    plan = Plan(4.5, SIZE, centre=(-6.0, -6.0))
    plan.beads(_paths(layer, lambda t: t != "Ironing" and t != "Top surface"), WALL)
    plan.beads(_paths(layer, _is("Top surface")), _rgb(PART, 0.62))
    plan.beads(_paths(layer, _is("Ironing")), LINE, thin=0.16)
    return plan.finish()


# --- Z contouring: the top of a half sphere, close to side on --------------------------------------


def _zaa(value):
    """A gentle ramp's top, from the front: the top surface's lines in ACCENT."""
    layers = _case("zaa_enabled", value)
    items = _items_z(layers, lambda t: True, lambda layer, p, z: ACCENT_BEAD if p["t"] == "Top surface" else PART)
    return Model(items, SIZE, target=(0.3, 0.0, 0.18), extent=1.05, az=15.0, el=11.0).finish()


# --- print infill first: one layer being printed --------------------------------------------------

ORDER_STEPS, ORDER_HOLD = 18, 4


def _infill_first(value):
    def picture(k, n):
        layer = _case("is_infill_first", value)[0]
        paths = _paths(layer, _not_moves)
        plan = Plan(16.5, GIF_SIZE)
        f = smooth(phase(k, 0, ORDER_STEPS)) if k < ORDER_STEPS else 1.0
        done = _partial(paths, f)
        for p in done:
            plan.beads([p], LINE if _walls(p["t"]) else WALL)
        if k < ORDER_STEPS and done:
            plan.dot(done[-1]["pts"][-1], 3.2, (235, 242, 242))
        return plan.finish()
    return picture


# --- one wall on the first layer, on top surfaces ---------------------------------------------------


def _one_wall(case, value):
    layer = _case(case, value)[0]
    plan = Plan(9.0, SIZE, centre=(-4.0, -4.0))
    plan.beads(_paths(layer, lambda t: not _walls(t)), _rgb(PART, 0.75), thin=0.9)
    plan.beads(_paths(layer, _walls), LINE)
    return plan.finish()


# --- avoid crossing walls: the travels of one layer of a C ----------------------------------------


def _crossing(value):
    layer = _case("reduce_crossing_wall", value)[0]
    plan = Plan(18.5, SIZE)
    plan.beads(_paths(layer, _not_moves), _rgb(PART, 0.7), edge=None)
    plan.beads(_paths(layer, _is("Travel")), _rgb(ACCENT, 1.45), thin=0.5)
    return plan.finish()


# --- bridges ----------------------------------------------------------------------------------------


def _deck(case, value, z):
    layers = _case(case, value)
    plan = Plan(23.0, SIZE)
    under = _layer(layers, z - 0.2)
    if under["z"] < z - 0.1:
        plan.beads(_paths(under, _is("Bridge")), _rgb(UNDER, 0.6), edge=None)
    layer = _layer(layers, z)
    plan.beads(_paths(layer, lambda t: t != "Bridge"), WALL)
    plan.beads(_paths(layer, _is("Bridge")), LINE, thin=0.75)
    return plan.finish()


def _internal_bridge(case, value):
    layers = _case(case, value)
    plan = Plan(16.0, SIZE, centre=(-3.0, -3.0))
    plan.beads(_paths(_layer(layers, 5.0), _is("Sparse infill")), _rgb(UNDER, 0.55), edge=None)
    layer = layers[-1]
    plan.beads(_paths(layer, _walls), WALL)
    plan.beads(_paths(layer, _is("Internal Bridge")), LINE, thin=0.75)
    return plan.finish()


def _overhang_angle(value):
    items = []
    for layer in _case("make_overhang_printable_angle", value):
        for p in _paths(layer, lambda t: True):
            grown = layer["z"] < 8.0 and _radius(p) > 3.4
            items.append((layer["z"], 0.2, p, ACCENT_BEAD if grown else PART))
    return Model(items, SIZE, target=(0.0, 0.0, 0.55), extent=2.2, el=6.0).finish()


# --- flow ratio: the beads of a surface in section --------------------------------------------------


def _flow(ratio):
    s = Section()
    s.poly([(0.0, 0.0), (1.0, 0.0), (1.0, 0.36), (0.0, 0.36)], _rgb(PART2, 0.8))
    pitch, h = 0.16, 0.13
    w = pitch * ratio
    for i in range(-1, 8):
        cx = 0.1 + i * pitch
        s.ellipse(cx, 0.36 + h * 0.5 * ratio ** 0.5, w * 0.62, h * 0.5 * ratio ** 0.6, LINE)
    return s.finish()


# --- small area flow compensation: the model's curve -------------------------------------------------

SAFC = [(0, 0), (0.2, 0.4444), (0.4, 0.6145), (0.6, 0.7059), (0.8, 0.7619), (1.5, 0.8571), (2, 0.8889),
        (3, 0.9231), (5, 0.9520), (10, 1)]


def _safc():
    s = Section()
    x = lambda v: 0.1 + 0.82 * math.sqrt(v / 10.0)  # noqa: E731  (a square-root axis spreads the short lengths)
    y = lambda v: 0.12 + 0.76 * v  # noqa: E731
    grid = _rgb(BED, 1.35)
    s.draw.line([s.xy(0.1, 0.12), s.xy(0.94, 0.12)], fill=grid, width=SS * 4)
    s.draw.line([s.xy(0.1, 0.12), s.xy(0.1, 0.92)], fill=grid, width=SS * 4)
    s.draw.line([s.xy(0.1, y(1.0)), s.xy(0.94, y(1.0))], fill=_rgb(PART, 0.55), width=SS * 2)
    pts = [s.xy(x(a), y(b)) for a, b in SAFC]
    s.draw.line(pts, fill=LINE, width=SS * 7, joint="curve")
    for px, py in pts:
        r = SS * 7
        s.draw.ellipse([px - r, py - r, px + r, py + r], fill=(235, 242, 242), outline=EDGE, width=SS * 2)
    return s.finish()


# --- the table -----------------------------------------------------------------------------------

for _v, _n in (("0.1", "low"), ("0.3", "high")):
    _fixed("layer_height_" + _n, lambda v=_v: _layer_height(v))
for _v, _n in (("0.12", "low"), ("0.32", "high")):
    _fixed("first_layer_height_" + _n, lambda v=_v: _first_layer(v))
for _v, _n in (("0.3", "narrow"), ("0.65", "wide")):
    _fixed("line_width_" + _n, lambda v=_v: _line_width(v))
for _v, _n in (("0", "off"), ("1", "on")):
    _fixed("staggered_inner_seams_" + _n, lambda v=_v: _staggered(v))
for _v, _n in (("0%", "low"), ("60%", "high")):
    _fixed("seam_gap_" + _n, lambda v=_v: _seam_gap(v))
for _v in ("none", "external", "all"):
    _fixed("scarf_" + _v, lambda v=_v: _scarf(v))
_fixed("scarf_section", lambda: _scarf_section())
_fixed("scarf_start_high", lambda: _scarf_section(start=0.5))
_fixed("scarf_length_short", lambda: _scarf_section(length=0.22))
_fixed("scarf_length_long", lambda: _scarf_section(length=0.8))
_fixed("scarf_steps_few", lambda: _scarf_section(steps=3))
_fixed("scarf_entire_loop", lambda: _scarf_section(entire=True))
for _v, _n in (("0.01", "fine"), ("0.3", "coarse")):
    _fixed("resolution_" + _n, lambda v=_v: _resolution(v))
for _v in ("disabled", "enabled", "sparse_infill"):
    _fixed("arc_fitting_" + _v, lambda v=_v: _arcs(v))
for _v, _n in (("-0.5", "negative"), ("0.5", "positive")):
    _fixed("xy_hole_" + _n, lambda v=_v: _xy("xy_hole_compensation", v))
    _fixed("xy_contour_" + _n, lambda v=_v: _xy("xy_contour_compensation", v))
for _v in ("off", "on", "layers"):
    _fixed("elephant_foot_" + _v, lambda v=_v: _elephant(v))
for _v in ("off", "on", "twisted"):
    _fixed("polyhole_" + _v, lambda v=_v: _polyhole(v))
for _v, _n in (("0.1", "low"), ("0.25", "high")):
    _fixed("ironing_spacing_" + _n, lambda v=_v: _ironing("ironing_spacing", v))
for _v, _n in (("0", "low"), ("1", "high")):
    _fixed("ironing_inset_" + _n, lambda v=_v: _ironing("ironing_inset", v))
for _v in ("0", "90"):
    _fixed("ironing_angle_" + _v, lambda v=_v: _ironing("ironing_angle", v))
for _v, _n in (("0", "off"), ("1", "on")):
    _fixed("zaa_" + _n, lambda v=_v: _zaa(v))
for _v, _n in (("0", "off"), ("1", "on")):
    FRAMES["infill_first_" + _n] = ORDER_STEPS + ORDER_HOLD
    globals()["infill_first_" + _n] = _infill_first(_v)
    _fixed("one_wall_first_layer_" + _n, lambda v=_v: _one_wall("only_one_wall_first_layer", v))
    _fixed("one_wall_top_" + _n, lambda v=_v: _one_wall("only_one_wall_top", v))
    _fixed("avoid_crossing_walls_" + _n, lambda v=_v: _crossing(v))
    _fixed("thick_internal_bridges_" + _n, lambda v=_v: _internal_bridge("thick_internal_bridges", v))
for _v, _n in (("50%", "low"), ("100%", "high")):
    _fixed("bridge_density_" + _n, lambda v=_v: _deck("bridge_density", v, 6.0))
    _fixed("internal_bridge_density_" + _n, lambda v=_v: _internal_bridge("internal_bridge_density", v))
for _v, _n in (("disabled", "off"), ("external_bridge_only", "on")):
    _fixed("extra_bridge_layer_" + _n, lambda v=_v: _deck("enable_extra_bridge_layer", v, 6.2))
for _v, _n in (("30", "low"), ("55", "high")):
    _fixed("overhang_printable_angle_" + _n, lambda v=_v: _overhang_angle(v))
_fixed("flow_low", lambda: _flow(0.8))
_fixed("flow_high", lambda: _flow(1.25))
_fixed("small_area_flow_model", _safc)
