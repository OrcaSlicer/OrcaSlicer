"""Pictures for the tooltips of the Process tab's Support page (src/slic3r/GUI/SettingTipsProcessSupport.cpp).

Drawn from real toolpaths, like settings_process.py, whose plan and model views this reuses:
settings_process_support_slice.py slices small test models once per value and keeps the extrusions. Three
ways of drawing them:

- a section: a cut through the part and its support along a vertical plane, seen from the side, every bead
  at its real width and layer height. The Z distances, the interface layers and the gaps around the support
  are a fraction of a millimetre high and only show like this.
- a plan: one layer from above (settings_process.Plan), for the line spacings and the first layer.
- a model: the part and its support in the plater's view (settings_process.Model), for what is supported
  and the shape of tree supports.

The two values of a setting share one model and one framing, so whatever differs between them is the
setting. What the setting places or changes is in ACCENT, the rest grey.
"""

import gzip
import json

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, GRID, PART, PART2, SIZE, SS
from .settings_process import EDGE, LINE, WALL, Model, Plan, _paths, _rgb
from .settings_process_support_slice import DATA

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case, value):
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data[case][value]


def _is_support(t):
    return t.startswith("Support")


def _is_interface(t):
    return t == "Support interface"


def _none(t):
    return False


BASE = _rgb(PART2, 1.15)  # the support's body, where the setting is about something else
ACC = _rgb(ACCENT, 1.25)


# --- sections: a cut along the plane y = 0, seen from the front -----------------------------------


class Section:
    """The plane y = 0 seen from the front, `span` mm across, centred on (x, z)."""

    def __init__(self, centre, span, size=SIZE):
        self.size = size
        self.px = size * SS * 2
        self.scale = self.px / span
        self.cx, self.cz = centre
        self.span = span
        self.image = Image.new("RGB", (self.px, self.px), _rgb(BED, 0.75))
        self.draw = ImageDraw.Draw(self.image)
        # The bed, under z = 0.
        _, top = self.xy(0.0, 0.0)
        if top < self.px:
            self.draw.rectangle([0, max(top, 0), self.px, self.px], fill=_rgb(GRID, 1.1))

    def xy(self, x, z):
        return ((x - self.cx) * self.scale + self.px / 2, self.px / 2 - (z - self.cz) * self.scale)

    def cut(self, layers, colour_of):
        """Every bead of the support the plane cuts near the window, coloured by colour_of(type):
        (x from, x to, z from, z to, colour)."""
        x0, x1 = self.cx - self.span / 2 - 1.0, self.cx + self.span / 2 + 1.0
        beads = []
        for layer in layers:
            z = layer["z"]
            if not (self.cz - self.span / 2 - 1.0 < z < self.cz + self.span / 2 + 1.0):
                continue
            for p in layer["paths"]:
                if not _is_support(p["t"]):
                    continue
                colour = colour_of(p["t"])
                pts = np.asarray(p["p"], float).reshape(-1, 2)
                for a, b in zip(pts[:-1], pts[1:]):
                    for lo, hi in _cut(a, b, p["w"] / 2.0):
                        if hi > x0 and lo < x1:
                            beads.append((lo, hi, z - p["h"], z, colour))
        return beads

    def part(self, layers, outline):
        """The part's layers, cut solid: outline(z) gives the part's extent along x at height z as a list of
        (from, to). Its own layers are read from the toolpaths, so a raft lifts it as it does the print.
        Returns them like cut() does."""
        heights = sorted({(layer["z"], p["h"]) for layer in layers for p in layer["paths"] if not _is_support(p["t"])})
        return [(lo, hi, z - h, z, WALL) for z, h in heights for lo, hi in outline(z - h / 2.0 - _lift(layers))]

    def beads(self, beads):
        for lo, hi, z0, z1, colour in beads:
            (ax, ay), (bx, by) = self.xy(lo, z1), self.xy(hi, z0)
            e = 1.2 * SS * 2
            rad = (by - ay) / 2.0
            self.draw.rounded_rectangle([ax - e, ay - e, bx + e, by + e], radius=rad + e, fill=EDGE)
            self.draw.rounded_rectangle([ax, ay, bx, by], radius=rad, fill=colour)

    def gap(self, beads, where):
        """Fills the gap between the support and the part in ACCENT, dimmed: the one over the support
        ("top"), under it ("bottom") or beside it, towards the part's axis ("side")."""
        support, part = beads
        lo, hi = min(b[0] for b in support), max(b[1] for b in support)
        top, bottom = max(b[3] for b in support), min(b[2] for b in support)
        if where == "top":
            box = (lo, hi, top, min(b[2] for b in part if b[2] >= top - 1e-3))
        elif where == "bottom":
            box = (lo, hi, max(b[3] for b in part if b[3] <= bottom + 1e-3), bottom)
        else:
            beside = [b for b in part if b[1] <= lo + 1e-3 and b[2] >= bottom - 1e-3 and b[3] <= top + 1e-3]
            box = (max(b[1] for b in beside), lo, bottom, top)
        (ax, ay), (bx, by) = self.xy(box[0], box[3]), self.xy(box[1], box[2])
        self.draw.rectangle([ax, ay, bx, by], fill=_rgb(ACCENT, 0.8))

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _cut(a, b, r):
    """The x interval where the plane y = 0 cuts a bead of radius r along the segment a-b, as a list: the
    extremes of the round caps at its ends and of the band between them, where the plane meets them."""
    (ax, ay), (bx, by) = a, b
    xs = []
    for cx, cy in ((ax, ay), (bx, by)):
        if abs(cy) <= r:
            h = np.sqrt(r * r - cy * cy)
            xs += [cx - h, cx + h]
    dx, dy = bx - ax, by - ay
    length = np.hypot(dx, dy)
    if length > 1e-9:
        nx, ny = -dy / length, dx / length
        # Points a + t (b - a) + s n with t in 0..1 and |s| <= r, on y = 0: the edges of that box.
        for s in (-r, r):
            if abs(dy) > 1e-12:
                t = -(ay + s * ny) / dy
                if 0.0 <= t <= 1.0:
                    xs.append(ax + t * dx + s * nx)
    return [(min(xs), max(xs))] if xs else []


def _lift(layers):
    """How far a raft lifts the part: the bottom of its first layer."""
    return min(layer["z"] - p["h"] for layer in layers for p in layer["paths"] if not _is_support(p["t"]))


def _outline(*parts):
    """A part made of upright cylinders and boxes, cut through its axis: parts are (radius, z from, z to)."""
    return lambda z: [(-r, r) for r, z0, z1 in parts if z0 <= z <= z1]


DISC = _outline((9, 0, 2), (2.5, 0, 7), (8, 6, 8))  # settings_process_support_slice.DISC
PLATE = _outline((5, 0, 2), (2, 0, 8), (9, 7, 9))
BLOCK = _outline((6, 0, 4))


# Where each section looks: (centre x, centre z), span in mm, and the part's outline.
SECTIONS = {
    "top_z": ((5.0, 6.2), 3.6, DISC),
    "bottom_z": ((5.0, 2.6), 3.6, DISC),
    "top_layers": ((5.0, 5.6), 4.4, DISC),
    "bottom_layers": ((5.0, 2.8), 4.0, DISC),
    "expansion": ((6.8, 4.2), 8.2, DISC),
    "xy_distance": ((4.2, 4.2), 8.2, DISC),
    "raft_contact": ((5.0, 1.0), 3.6, BLOCK),
    "on_plate": ((4.8, 4.6), 10.4, PLATE),
}


def _section(case, value, accent=_is_support, gap=None):
    """A section, with the support's beads that `accent(type)` picks in ACCENT, and the gap `gap` (see
    Section.gap) too if given."""
    def picture(k, n):
        centre, span, outline = SECTIONS[case]
        s = Section(centre, span)
        layers = _case(case, value)
        support = s.cut(layers, lambda t: ACC if accent(t) else BASE)
        part = s.part(layers, outline)
        if gap:
            s.gap((support, part), gap)
        s.beads(part)
        s.beads(support)
        return s.finish()
    return picture


# --- plans ----------------------------------------------------------------------------------------


def _plan(case, value, span, test, centre=(0.0, 0.0)):
    """The top layer kept from above: what `test` picks in ACCENT, the rest of the layer grey."""
    def picture(k, n):
        top = _case(case, value)[-1]
        plan = Plan(span, SIZE, centre=centre)
        plan.beads(_paths(top, lambda t: not test(t)), WALL)
        plan.beads(_paths(top, test), LINE)
        return plan.finish()
    return picture


def _support_layer(case, value, span, z=None, test=_is_support, centre=(0.0, 0.0)):
    """The layer with support that `test` picks nearest height z (the top one if z is None) from above, with
    the part's paths of that layer grey."""
    def picture(k, n):
        layers = [layer for layer in _case(case, value) if any(test(p["t"]) for p in layer["paths"])]
        layer = layers[-1] if z is None else min(layers, key=lambda layer: abs(layer["z"] - z))
        plan = Plan(span, SIZE, centre=centre)
        plan.beads(_paths(layer, lambda t: not _is_support(t)), WALL)
        plan.beads(_paths(layer, test), LINE)
        return plan.finish()
    return picture


def _ironing(case, value):
    """A patch of the top interface close up, the ironing's lines over it: they are a tenth of a millimetre
    apart."""
    def picture(k, n):
        layers = _case(case, value)
        top = max((layer for layer in layers if any(_is_interface(p["t"]) for p in layer["paths"])), key=lambda l: l["z"])
        plan = Plan(2.6, SIZE, centre=(-4.0, -4.0))
        plan.beads(_paths(top, _is_interface), _rgb(PART, 0.75), edge=EDGE)
        for layer in layers:
            plan.beads(_paths(layer, lambda t: t == "Ironing"), LINE, edge=None, thin=0.12)
        return plan.finish()
    return picture


# --- models ---------------------------------------------------------------------------------------


def _model(case, value, view, accent=_is_support):
    """The part and its support in the plater's view, each bead at its own layer height: what `accent(type)`
    picks in ACCENT, the rest grey."""
    def picture(k, n):
        items = [(layer["z"], p["h"], p, ACCENT * 1.15 if accent(p["t"]) else PART)
                 for layer in _case(case, value) for p in _paths(layer, lambda t: True)]
        return Model(items, SIZE, **view).finish()
    return picture


def _brim(case, value):
    """A tree support's brim: its first layer in ACCENT, the rest of the support in a grey of its own."""
    def picture(k, n):
        items = []
        for layer in _case(case, value):
            for p in _paths(layer, lambda t: True):
                colour = (ACCENT * 1.15 if layer["z"] < 0.25 else PART2 * 1.1) if _is_support(p["t"]) else PART
                items.append((layer["z"], p["h"], p, colour))
        return Model(items, SIZE, **BRIM_VIEW).finish()
    return picture


# The tall mushroom of settings_process.py's support_style pictures, framed the same.
TREE_VIEW = dict(target=(0.0, 0.0, 0.85), extent=2.75, el=20.0)
BALL_VIEW = dict(target=(0.0, 0.0, 0.85), extent=2.3, el=15.0)
BRIDGE_VIEW = dict(target=(0.0, 0.0, 0.4), extent=2.6, el=22.0)
BLOCK_VIEW = dict(target=(0.0, 0.0, 0.15), extent=2.2, el=30.0)
BRIM_VIEW = dict(target=(0.0, 0.0, 0.7), extent=3.1, el=32.0)


# --- the table ------------------------------------------------------------------------------------

FRAMES = {}


def _add(name, fn):
    FRAMES[name] = 0
    globals()[name] = fn


def _slug(value):
    return value.replace(".", "_").replace("%", "")


def _pair(case, values, make):
    for v in values:
        _add("support_%s_%s" % (case, _slug(v)), make(v))


# Sections.
_pair("top_z", ("0.1", "0.3"), lambda v: _section("top_z", v, _none, "top"))
_pair("bottom_z", ("0.1", "0.5"), lambda v: _section("bottom_z", v, _none, "bottom"))
_pair("top_layers", ("0", "3"), lambda v: _section("top_layers", v, _is_interface))
_pair("bottom_layers", ("0", "2"), lambda v: _section("bottom_layers", v, _is_interface))
_pair("expansion", ("0", "1.5"), lambda v: _section("expansion", v))
_pair("xy_distance", ("0.35", "1.5"), lambda v: _section("xy_distance", v, _none, "side"))
_pair("raft_contact", ("0.1", "0.3"), lambda v: _section("raft_contact", v, _none, "top"))
_pair("on_plate", ("0", "1"), lambda v: _section("on_plate", v))

# Plans.
_pair("base_spacing", ("1", "4"), lambda v: _plan("base_spacing", v, 21.0, _is_support))
_pair("angle", ("0", "45"), lambda v: _plan("angle", v, 21.0, _is_support))
_pair("interface_spacing", ("0", "1"), lambda v: _plan("interface_spacing", v, 21.0, _is_interface))
_pair("bottom_spacing", ("0", "1"), lambda v: _support_layer("bottom_spacing", v, 21.0, 2.0, _is_interface))
_pair("ironing", ("0", "1"), lambda v: _ironing("ironing", v))
_pair("ironing_spacing", ("0.1", "0.3"), lambda v: _ironing("ironing_spacing", v))
_pair("first_density", ("30%", "90%"), lambda v: _plan("first_density", v, 20.0, _is_support))
_pair("first_expansion", ("0", "3"), lambda v: _plan("first_expansion", v, 26.0, _is_support))
_pair("first_gap", ("0.2", "1.5"), lambda v: _plan("first_gap", v, 7.0, _is_support, centre=(4.5, 0.0)))
_pair("wall_count", ("1", "2"), lambda v: _support_layer("wall_count", v, 22.0, 10.0))
_pair("tip_diameter", ("0.8", "2"), lambda v: _support_layer("tip_diameter", v, 21.0))
_pair("branch_distance_organic", ("1", "4"), lambda v: _support_layer("branch_distance_organic", v, 21.0))
_pair("top_rate", ("10%", "35%"), lambda v: _support_layer("top_rate", v, 21.0))
_pair("branch_distance", ("2", "6"), lambda v: _support_layer("branch_distance", v, 21.0, 12.0))

# Models.
_pair("enable", ("0",), lambda v: _model("enable", v, TREE_VIEW))
_pair("threshold", ("30", "60"), lambda v: _model("threshold", v, BALL_VIEW))
_pair("overlap", ("10%", "90%"), lambda v: _model("overlap", v, BALL_VIEW))
_pair("raft_layers", ("0", "3"), lambda v: _model("raft_layers", v, BLOCK_VIEW))
_pair("raft_expansion", ("0", "3"), lambda v: _model("raft_expansion", v, BLOCK_VIEW))
_pair("bridge_no_support", ("0", "1"), lambda v: _model("bridge_no_support", v, BRIDGE_VIEW))
for _c, _vs in (("branch_diameter_organic", ("2", "5")), ("diameter_angle", ("0", "10")), ("branch_angle_organic", ("20", "55")),
                ("angle_slow", ("10", "35")), ("branch_diameter", ("2", "6"))):
    _pair(_c, _vs, lambda v, c=_c: _model(c, v, TREE_VIEW))
_pair("tree_brim", ("0", "5"), lambda v: _brim("tree_brim", v))
