"""Pictures for the tooltips of the Process tab's Multimaterial and Others pages
(src/slic3r/GUI/SettingTipsProcessOthers.cpp).

Drawn the way settings_process.py draws its own, as a plan (one layer from straight above) or a model
(many layers as round beads in the plater's view), from toolpaths OrcaSlicer sliced for each value:
settings_process_others_slice.py. What the setting places or changes is in ACCENT, the rest grey. In a
picture of two filaments, the second filament is the ACCENT one.

A few settings act on something no G-code line shows (a temperature over time, the order of tool
changes), and are drawn as plain diagrams in the same palette instead.

The two values of a comparison share one model and one framing, so whatever differs between the two
pictures is the setting.
"""

import gzip
import json
import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, PART, PART2, SIZE, SS
from .settings_process import EDGE, LINE, WALL, Model, Plan, _clip, _paths, _rgb
from .settings_process_others_slice import DATA

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case, value):
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data[case][value]


def _layer(layers, z):
    return min(layers, key=lambda layer: abs(layer["z"] - z))


ACC = ACCENT * 1.15  # a bead's ACCENT: beads are lit from the side, which darkens them


def _items(layers, colour_of, height=None):
    """Beads of every path, coloured by colour_of(path, layer) (None leaves the path out)."""
    out = []
    for layer in layers:
        for p in _paths(layer, lambda t: True):
            c = colour_of(p, layer)
            if c is not None:
                out.append((layer["z"], height or p.get("h", 0.2), p, c))
    return out


def _fit(items, scale=0.1, el=30.0, az=35.0, margin=1.12, lift=0.0):
    """A view that frames `items`: the camera's target and extent around their projected outline."""
    pts = []
    for z, h, p, _ in items:
        q = p["pts"][:: max(1, len(p["pts"]) // 24)]
        for zz in (z - h, z):
            pts.append(np.column_stack([q, np.full(len(q), zz)]))
    pts = np.concatenate(pts) * scale
    a, e = math.radians(az), math.radians(el)
    c = np.array([math.sin(a) * math.cos(e), -math.cos(a) * math.cos(e), math.sin(e)])
    right = np.cross([0.0, 0.0, 1.0], c)
    right /= np.linalg.norm(right)
    up = np.cross(c, right)
    u, v = pts @ right, pts @ up
    centre = right * 0.5 * (u.min() + u.max()) + up * (0.5 * (v.min() + v.max()) + lift)
    extent = max(u.max() - u.min(), v.max() - v.min()) * margin
    return dict(target=tuple(centre), extent=extent, el=el, az=az)


def _model(items, view):
    return Model(items, SIZE, **view).finish()


FRAMES = {}


def _add(name, fn, frames=0):
    FRAMES[name] = frames
    globals()[name] = fn


def _pair(prefix, values, draw):
    """One picture per value of a comparison: prefix_<value>, drawn by draw(value)."""
    for v in values:
        _add("%s_%s" % (prefix, v.replace(".", "_").replace("-", "m")), draw(v))


# --- Multimaterial: the prime tower ----------------------------------------------------------------


def _tower_colour(p, layer):
    return ACC if p["t"] == "Prime tower" else PART


def _tower_model(value, view=None, colour=_tower_colour, case="tower"):
    def picture(k, n):
        items = _items(_case(case, value), colour)
        return _model(items, view or _fit(items, el=30.0))
    return picture


def _tower_items_only(value, case="tower"):
    return _items(_case(case, value), lambda p, layer: ACC if p["t"] == "Prime tower" else None)


def _tower_close(value, ref, case="tower", el=38.0):
    """The tower alone, close up, framed on the tower of `ref` so two values compare."""
    def picture(k, n):
        view = _fit(_tower_items_only(ref, case), el=el, margin=1.25)
        return _model(_tower_items_only(value, case), view)
    return picture


def _tower_plan(value, z, span, ref=None, case="tower", thin=1.0):
    """One layer of the tower from above, centred on the tower of `ref`."""
    def picture(k, n):
        layer = _layer(_case(case, ref or value), z)
        pts = np.concatenate([p["pts"] for p in _paths(layer, lambda t: t == "Prime tower")])
        plan = Plan(span, SIZE, centre=0.5 * (pts.min(axis=0) + pts.max(axis=0)))
        layer = _layer(_case(case, value), z)
        plan.beads(_paths(layer, lambda t: t == "Prime tower"), LINE, thin=thin)
        return plan.finish()
    return picture


def _plate_plan(value, z, span, case="tower", centre=(4.0, -2.0)):
    """The whole layer from above: the parts grey, the tower in ACCENT."""
    def picture(k, n):
        layer = _layer(_case(case, value), z)
        plan = Plan(span, SIZE, centre=centre)
        plan.beads(_paths(layer, lambda t: t != "Prime tower"), WALL)
        plan.beads(_paths(layer, lambda t: t == "Prime tower"), LINE)
        return plan.finish()
    return picture


def _tall(value, ref):
    def picture(k, n):
        items = _items(_case("tower_tall", value), lambda p, layer: ACC)
        view = _fit(_items(_case("tower_tall", ref), lambda p, layer: ACC), el=22.0, margin=1.15)
        return _model(items, view)
    return picture


def _sparse_colour(p, layer):
    return ACC if p["t"] == "Prime tower" else PART


def _skip(value):
    """The tower's right wall, where the nozzle enters to purge, close up."""
    def picture(k, n):
        layer = _layer(_case("tower", "skip_" + value), 1.0)
        plan = Plan(9.0, SIZE, centre=(19.5, 0.5))
        plan.beads(_paths(layer, lambda t: t == "Prime tower"), LINE)
        return plan.finish()
    return picture


def _no_sparse(value):
    """The part and its tower. The G-code gives each tower layer the height of the part's layer it is
    printed with; skipping the sparse layers, the nozzle comes down to print it on the tower's own top
    instead, so the tower is drawn stacked: each of its layers right on the one before."""
    def picture(k, n):
        items = _items(_case("tower_sparse", "nosparse_" + value), lambda p, layer: PART if p["t"] != "Prime tower" else None)
        tower = [layer for layer in _case("tower_sparse", "nosparse_" + value) if any(p["t"] == "Prime tower" for p in layer["paths"])]
        for i, layer in enumerate(tower):
            z = layer["z"] if value == "0" else 0.2 * (i + 1)
            items += [(z, 0.2, p, ACC) for p in _paths(layer, lambda t: t == "Prime tower")]
        return _model(items, _fit(_items(_case("tower_sparse", "nosparse_0"), _sparse_colour)))
    return picture


def _combine(value):
    """The layers of the part and of the tower seen edge on, as a section: thin layers where the tower
    purges, and above them, combined, one thick layer in the time of several of the part's."""
    def picture(k, n):
        layers = _case("tower_sparse", "combine_" + value)
        img = Image.new("RGB", (SIZE * SS, SIZE * SS), _rgb(BED, 0.75))
        draw = ImageDraw.Draw(img)
        z0, z1 = 2.4, 5.6  # mm of height shown: the top of the bottom tool changes and the sparse layers above
        px = SIZE * SS / (z1 - z0)
        for x0, x1, test, colour in ((0.06, 0.40, lambda t: t != "Prime tower", _rgb(PART)),
                                     (0.56, 0.94, lambda t: t == "Prime tower", _rgb(ACCENT, 1.25))):
            for layer in layers:
                hs = [p["h"] for p in layer["paths"] if test(p["t"])]
                if not hs:
                    continue
                top, h = layer["z"], max(hs)
                ya, yb = (z1 - top) * px, (z1 - top + h) * px
                if yb < 0 or ya > SIZE * SS:
                    continue
                r = min(h * px * 0.45, 0.03 * SIZE * SS)
                draw.rounded_rectangle([x0 * SIZE * SS, ya + 1.5 * SS, x1 * SIZE * SS, yb - 1.5 * SS], radius=r, fill=colour)
        return np.asarray(img.resize((SIZE, SIZE), Image.LANCZOS))
    return picture


_add("prime_tower_off", _tower_model("enable_0", _fit(_items(_case("tower", "enable_1"), _tower_colour))))
_add("prime_tower_on", _tower_model("enable_1"))
for _v in ("rectangle", "cone", "rib"):
    _add("tower_wall_" + _v, _tall("wall_" + _v, "cone_60"))
_add("tower_cone_60", _tall("cone_60", "cone_60"))  # 30° is the default: tower_wall_cone.png
_pair("tower_brim", ("0", "4"), lambda v: _tower_close("brim_" + v, "brim_4", el=45.0))
_pair("tower_skip", ("0", "1"), lambda v: _skip(v))
_pair("tower_rib_length", ("0", "12"), lambda v: _tower_plan("riblen_" + v, 4.0, 40.0, ref="riblen_12"))
_pair("tower_rib_width", ("4", "14"), lambda v: _tower_plan("ribw_" + v, 4.0, 36.0, ref="ribw_14"))
_pair("tower_fillet", ("0", "1"), lambda v: _tower_plan("fillet_" + v, 4.0, 36.0, ref="fillet_1"))
_pair("tower_rotation", ("0", "30"), lambda v: _plate_plan("rot_" + v, 4.0, 46.0, centre=(6.0, -2.0)))
_pair("tower_width", ("12", "30"), lambda v: _plate_plan("width_" + v, 4.0, 52.0, centre=(10.0, -4.0)))
_pair("tower_spacing", ("100", "200"), lambda v: _tower_plan("spacing_" + v, 2.0, 30.0, ref="spacing_200"))
_pair("tower_sparse", ("0", "1"), _no_sparse)
_pair("tower_bridging", ("4", "16"), lambda v: _tower_plan("bridging_" + v, 5.0, 26.0, case="tower_sparse"))
_pair("tower_combine", ("0", "1"), _combine)


# --- Multimaterial: where the purge goes -----------------------------------------------------------
#
# Two parts, the big one in the first filament, the small one in the second (ACCENT), from above:
# without a flush option every purge is in the tower; flushed into the infill, the big part's infill
# takes some of the second filament; flushed into the objects, whole parts are printed with purge.


def _tool_colour(p):
    return LINE if p["f"] == 1 else WALL


def _flush(value):
    def picture(k, n):
        layer = _layer(_case("flush", value), 3.0)
        plan = Plan(38.0, SIZE, centre=(4.0, 1.0))
        for p in _paths(layer, lambda t: True):
            plan.beads([p], _tool_colour(p))
        return plan.finish()
    return picture


for _v in ("none", "infill"):
    _add("flush_" + _v, _flush(_v))


def _flush_objects(k, n):
    """A part kept for the purge, beside the part it is printed with: it takes whatever the nozzle is
    clearing at each change, so it comes out in bands of both filaments, and there is no tower. Drawn,
    not sliced: "Flush into this object" is set per object, which the slicer's command line cannot."""
    layers = _case("print_sequence", "parts")
    items = []
    for i, layer in enumerate(layers):
        for p in _paths(layer, lambda t: True):
            purge = p["pts"][:, 0].mean() > 0.0
            band = ACC if (i // 3) % 2 == 0 else PART2 * 1.25
            items.append((layer["z"], 0.2, p, band if purge else PART))
    return _model(items, _fit(items))


_add("flush_objects", _flush_objects)


# --- Multimaterial: interlocking -------------------------------------------------------------------
#
# Two blocks merged into one part, the left in the first filament, the right in the second. The second
# is drawn alone, seen from the left, so the beams it pushes into the first stand out of its face.

INTERLOCK_VIEW = dict(target=(0.22, 0.0, 0.28), extent=1.6, el=24.0, az=-50.0)


def _interlock(value):
    def picture(k, n):
        items = _items(_case("interlock", value), lambda p, layer: ACC if p["f"] == 1 else None)
        return _model(items, INTERLOCK_VIEW)
    return picture


_pair("interlock", ("beam_0", "beam_1"), lambda v: _interlock(v))
_pair("interlock_width", ("0.5", "1.6"), lambda v: _interlock("width_" + v))
_pair("interlock_orient", ("0", "45"), lambda v: _interlock("orient_" + v))
_pair("interlock_layers", ("1", "4"), lambda v: _interlock("layers_" + v))
_pair("interlock_depth", ("1", "4"), lambda v: _interlock("depth_" + v))
_pair("interlock_avoid", ("0", "3"), lambda v: _interlock("avoid_" + v))


# --- Multimaterial: interface shells ---------------------------------------------------------------


def _interface_shells(value):
    """Two stacked blocks cut open at the front: solid layers where the two filaments meet, or not."""
    def picture(k, n):
        items = []
        for layer in _case("interface_shells", "shells_" + value):
            for p in _clip(_paths(layer, lambda t: True), (0.0, -1.0), 0.0):
                items.append((layer["z"], 0.2, p, ACC if p["f"] == 1 else PART))
        return _model(items, dict(target=(0.0, 0.25, 0.5), extent=2.3, el=22.0))
    return picture


_pair("interface_shells", ("0", "1"), _interface_shells)


# --- diagrams: temperatures over time, the order of tool changes, the spiral's flow ---------------


class Chart:
    """A plain chart in the pictures' palette: no text, a faint grid, lines and blocks."""

    def __init__(self, size=SIZE):
        self.size = size
        self.px = size * SS * 2
        self.image = Image.new("RGB", (self.px, self.px), _rgb(BED, 0.75))
        self.draw = ImageDraw.Draw(self.image)
        self.x0, self.x1, self.y0, self.y1 = 0.10, 0.92, 0.84, 0.14  # the plot area, as fractions

    def xy(self, x, y):
        return ((self.x0 + (self.x1 - self.x0) * x) * self.px, (self.y0 + (self.y1 - self.y0) * y) * self.px)

    def grid(self, rows=4):
        for i in range(rows + 1):
            self.line([(0, i / rows), (1, i / rows)], _rgb(BED, 1.35), 1.0)
        self.line([(0, 0), (0, 1)], _rgb(PART, 0.6), 1.5)
        self.line([(0, 0), (1, 0)], _rgb(PART, 0.6), 1.5)

    def line(self, pts, colour, width=3.0):
        self.draw.line([self.xy(x, y) for x, y in pts], fill=colour, width=int(width * SS * 2), joint="curve")

    def band(self, xa, xb, colour, y=-0.06, h=0.05):
        a, b = self.xy(xa, y), self.xy(xb, y - h)
        self.draw.rectangle([a[0] + SS * 2, a[1], b[0] - SS * 2, b[1]], fill=colour)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _ooze(drop, preheat, on=True):
    """Two filaments' nozzle temperatures through three tool changes (the bands under the chart say which
    one prints when): the idle one drops by `drop` and heats up again `preheat` before its turn."""
    def picture(k, n):
        chart = Chart()
        chart.grid()
        turns = [(0.0, 0.33, 0), (0.33, 0.66, 1), (0.66, 1.0, 0)]
        hot = 0.85
        for tool, colour in ((0, _rgb(PART)), (1, _rgb(ACCENT, 1.25))):
            pts = []
            for i in range(201):
                t = i / 200.0
                active = next(f for a, b, f in turns if a <= t <= b) == tool
                if not on or active:
                    temp = hot
                else:
                    start = max(a for a, b, f in turns if a <= t)  # when it went idle
                    nxt = min([a for a, b, f in turns if a > t and f == tool] + [1.4])
                    cool = hot - drop * (1.0 - math.exp(-(t - start) / 0.04))
                    if t > nxt - preheat:  # heating up for its next turn
                        cool = cool + (hot - cool) * min(1.0, (t - (nxt - preheat)) / preheat)
                    temp = cool
                pts.append((t, temp + (0.012 if tool else 0.0)))
            chart.line(pts, colour)
        for a, b, f in turns:
            chart.band(a, b, _rgb(ACCENT, 1.25) if f else _rgb(PART))
        return chart.finish()
    return picture


_add("ooze_off", _ooze(0.0, 0.0, on=False))
_add("ooze_on", _ooze(0.45, 0.12))
_add("ooze_delta_small", _ooze(0.18, 0.12))
_add("ooze_preheat_short", _ooze(0.45, 0.04))
_add("ooze_preheat_long", _ooze(0.45, 0.24))

TOOLS = [_rgb(ACCENT, 1.25), _rgb(PART), _rgb(PART2, 1.1)]


def _toolchanges(rows):
    """Layers from the bottom up, each the filaments it prints in order, joined by the nozzle's way."""
    def picture(k, n):
        img = Image.new("RGB", (SIZE * SS, SIZE * SS), _rgb(BED, 0.75))
        draw = ImageDraw.Draw(img)
        s = SIZE * SS
        cell, gap = 0.17 * s, 0.11 * s
        x0 = 0.5 * s - 1.5 * cell - gap
        for r, order in enumerate(rows):
            y = s * 0.86 - r * (cell + 0.06 * s) - cell
            centres = [(x0 + i * (cell + gap) + cell / 2, y + cell / 2) for i in range(len(order))]
            draw.line(centres, fill=_rgb(PART, 0.55), width=int(2.2 * SS))
            for (cx, cy), tool in zip(centres, order):
                draw.rounded_rectangle([cx - cell / 2, cy - cell / 2, cx + cell / 2, cy + cell / 2], radius=0.12 * cell,
                                       fill=TOOLS[tool], outline=_rgb(BED, 0.5), width=SS)
            if r + 1 < len(rows):  # the step up to the next layer
                nxt = s * 0.86 - (r + 1) * (cell + 0.06 * s) - cell / 2
                draw.line([centres[-1], (centres[-1][0] + cell * 0.75, centres[-1][1]),
                           (centres[-1][0] + cell * 0.75, nxt), (x0 + cell / 2, nxt)][:3],
                          fill=_rgb(PART, 0.55), width=int(2.2 * SS))
        return np.asarray(img.resize((SIZE, SIZE), Image.LANCZOS))
    return picture


_add("toolchange_default", _toolchanges([[0, 1, 2], [2, 0, 1], [1, 2, 0], [0, 1, 2]]))
_add("toolchange_cyclic", _toolchanges([[0, 1, 2]] * 4))
_add("toolchange_first_layer_off", _toolchanges([[1, 2, 0], [0, 1, 2], [0, 1, 2], [0, 1, 2]]))


def _spiral_flow(start, finish):
    """The flow along the spiral's first loop (left) and last loop (right), between the plain layers."""
    def picture(k, n):
        chart = Chart()
        chart.grid()
        pts = [(0.0, 1.0 if start is None else 0.0)]
        if start is not None:
            pts += [(0.0, start), (0.3, 1.0)]
        pts += [(0.7, 1.0)]
        pts += [(1.0, finish)] if finish is not None else [(1.0, 1.0)]
        chart.line([(0.0, 0.0), (0.0, 0.0)], _rgb(PART), 0.1)
        chart.line(pts if start is not None else pts[1:], _rgb(ACCENT, 1.25))
        chart.band(0.0, 0.3, _rgb(ACCENT, 1.25) if start is not None else _rgb(PART))
        chart.band(0.3, 0.7, _rgb(PART))
        chart.band(0.7, 1.0, _rgb(ACCENT, 1.25) if finish is not None else _rgb(PART))
        return chart.finish()
    return picture


_add("spiral_start_0", _spiral_flow(0.0, None))
_add("spiral_start_half", _spiral_flow(0.5, None))
_add("spiral_finish_0", _spiral_flow(None, 0.0))
_add("spiral_finish_half", _spiral_flow(None, 0.5))


# --- Others: skirt ---------------------------------------------------------------------------------


def _kind_colour(kind):
    return lambda p, layer: ACC if p["t"] == kind else PART


def _compare(case, value, ref, kind, el=35.0, margin=1.08):
    """A model of `value` with the extrusions of `kind` in ACCENT, framed on `ref`, the larger value."""
    def picture(k, n):
        view = _fit(_items(_case(case, ref), _kind_colour(kind)), el=el, margin=margin)
        return _model(_items(_case(case, value), _kind_colour(kind)), view)
    return picture


_pair("skirt_loops", ("1", "3"), lambda v: _compare("skirt", "loops_" + v, "loops_3", "Skirt"))
_pair("skirt_distance", ("1", "8"), lambda v: _compare("skirt", "dist_" + v, "dist_8", "Skirt"))
_pair("skirt_height", ("1", "5"), lambda v: _compare("skirt", "height_" + v, "height_5", "Skirt"))
_pair("skirt_min_length", ("0", "6"), lambda v: _compare("skirt", "minlen_" + v, "minlen_6", "Skirt"))
_pair("skirt_single_loop", ("0", "1"), lambda v: _compare("skirt", "single_" + v, "single_0", "Skirt", el=50.0))
_pair("skirt_type", ("combined", "perobject"), lambda v: _compare("skirt_type", "type_" + v, "type_combined", "Skirt", el=45.0))


def _skirt_start(value):
    """The first layer from above, with the point where the skirt starts."""
    def picture(k, n):
        layer = _case("skirt", "angle_" + value)[0]
        plan = Plan(22.0, SIZE)
        plan.beads(_paths(layer, lambda t: t != "Skirt"), WALL)
        skirt = _paths(layer, lambda t: t == "Skirt")
        plan.beads(skirt, LINE)
        plan.dot(skirt[0]["pts"][0], 4.0, (235, 242, 242))
        return plan.finish()
    return picture


_pair("skirt_start", ("m135", "45"), lambda v: _skirt_start(v.replace("m", "-")))


# --- Others: brim ----------------------------------------------------------------------------------

BRIM_VIEW = dict(target=(0.0, 0.0, 0.15), extent=2.6, el=52.0)

_pair("brim_width", ("2", "8"), lambda v: (lambda k, n: _model(_items(_case("brim", "width_" + v), _kind_colour("Brim")), BRIM_VIEW)))


def _brim_edge(value):
    """The first layer where the brim meets the part's wall, close up."""
    def picture(k, n):
        layer = _case("brim", value)[0]
        plan = Plan(5.0, SIZE, centre=(8.4, 0.0))
        plan.beads(_paths(layer, lambda t: t != "Brim"), WALL)
        plan.beads(_paths(layer, lambda t: t == "Brim"), LINE)
        return plan.finish()
    return picture


_pair("brim_gap", ("0", "1"), lambda v: _brim_edge("gap_" + v))
_pair("brim_efc", ("0", "1"), lambda v: _brim_edge("efc_" + v))


def _first_layer_plan(case, value, span, kind="Brim"):
    def picture(k, n):
        layer = _case(case, value)[0]
        plan = Plan(span, SIZE)
        plan.beads(_paths(layer, lambda t: t != kind), WALL)
        plan.beads(_paths(layer, lambda t: t == kind), LINE)
        return plan.finish()
    return picture


_pair("combine_brims", ("0", "1"), lambda v: _first_layer_plan("combine_brims", "combine_" + v, 30.0))
_pair("brim_ears_angle", ("60", "130"), lambda v: _first_layer_plan("brim_ears", "angle_" + v, 30.0))


# --- Others: special modes -------------------------------------------------------------------------

for _v in ("regular", "even_odd", "close_holes"):
    _add("slicing_mode_" + _v, (lambda v: lambda k, n: _model(
        _items(_case("slicing_mode", v), lambda p, layer: ACC if "wall" in p["t"] else PART),
        dict(target=(0.0, 0.0, 0.2), extent=2.1, el=50.0)))(_v))


def _print_order(value):
    """One layer of nine small parts and the nozzle's way from one to the next, from the first part
    (the white dot) on."""
    def picture(k, n):
        layer = _case("print_order", value)[0]
        walls = _paths(layer, lambda t: True)
        plan = Plan(34.0, SIZE)
        starts = [p["pts"][0] for p in walls]
        plan.draw.line(plan.xy(starts), fill=LINE, width=int(2.4 * SS * 2), joint="curve")
        plan.beads(walls, WALL)
        for i, s in enumerate(starts):
            plan.dot(s, 3.2 if i == 0 else 2.0, (235, 242, 242) if i == 0 else LINE)
        return plan.finish()
    return picture


# Best of and Snake take the same way as Default on a layout this small, so only these two are drawn.
for _v in ("default", "as_obj_list"):
    _add("print_order_" + _v, _print_order(_v))


def _print_sequence(by_object):
    """Two parts part-way through the print: both half done, or the first done before the second starts.
    The layer printed last is in ACCENT."""
    def picture(k, n):
        layers = _case("print_sequence", "parts")
        items = []
        for layer in layers:
            for p in _paths(layer, lambda t: True):
                first = p["pts"][:, 0].mean() < 0.0
                top = 8.0 if (by_object and first) else (1.6 if by_object else 4.0)
                if layer["z"] <= top + 1e-6:
                    live = abs(layer["z"] - top) < 1e-6 and not (by_object and first)
                    items.append((layer["z"], 0.2, p, ACC if live else PART))
        return _model(items, _fit(_items(layers, lambda p, layer: PART)))
    return picture


_add("print_sequence_by_layer", _print_sequence(False))
_add("print_sequence_by_object", _print_sequence(True))

_pair("spiral", ("0", "1"), lambda v: (lambda k, n: _model(
    _items(_case("spiral", "spiral_" + v), lambda p, layer: ACC if "wall" in p["t"] else PART),
    dict(target=(0.0, 0.0, 0.45), extent=2.1, el=42.0))))


# --- Others: fuzzy skin ----------------------------------------------------------------------------

FUZZY_VIEW = dict(target=(0.0, 0.0, 0.4), extent=1.85, el=30.0)


def _fuzzy(value):
    def picture(k, n):
        items = _items(_case("fuzzy", value), lambda p, layer: ACC if "wall" in p["t"] else PART)
        return _model(items, FUZZY_VIEW)
    return picture


def _fuzzy_plan(value, z=4.0):
    """A stretch of one layer's outer wall from above, close up."""
    def picture(k, n):
        layer = _layer(_case("fuzzy", value), z)
        plan = Plan(4.0, SIZE, centre=(0.0, -6.6))
        plan.beads(_paths(layer, lambda t: t == "Inner wall"), WALL)
        plan.beads(_paths(layer, lambda t: t == "Outer wall"), LINE)
        return plan.finish()
    return picture


_pair("fuzzy_point_distance", ("0.2", "1.2"), lambda v: _fuzzy_plan("pd_" + v))
_pair("fuzzy_thickness", ("0.15", "0.6"), lambda v: _fuzzy_plan("th_" + v))
for _name, _key, _values in (("fuzzy_scale", "scale", ("0.4", "3")), ("fuzzy_octaves", "oct", ("1", "8")),
                             ("fuzzy_persistence", "pers", ("0.15", "0.9")), ("fuzzy_ripples", "rip", ("6", "30")),
                             ("fuzzy_ripple_offset", "ripoff", ("0", "50")), ("fuzzy_ripple_layers", "riplayers", ("1", "5"))):
    _pair(_name, _values, (lambda key: lambda v: _fuzzy("%s_%s" % (key, v)))(_key))
_pair("fuzzy_first_layer", ("0", "1"), lambda v: _fuzzy_plan("first_" + v, 0.2))


def _fuzzy_mode(value):
    """A stretch of the outer wall close up, each segment at the width its filament fills: Displacement
    moves the line, Extrusion widens and narrows it, Combined does both."""
    def picture(k, n):
        layer = _case("fuzzy_mode", value)[0]
        plan = Plan(4.0, SIZE, centre=(0.0, -6.6))
        plan.beads(_paths(layer, lambda t: t == "Inner wall"), WALL)
        # Extrusion mode writes a width per segment, so the wall comes in many short paths: rims for all
        # of them first, then the fill, or each rim would cut the line where two paths meet.
        segments = [(a, b, w) for p in _paths(layer, lambda t: t == "Outer wall")
                    for a, b, w in zip(plan.xy(p["pts"])[:-1], plan.xy(p["pts"])[1:], p["e"])]
        for extra, c in ((1.5 * SS * 2, EDGE), (0.0, LINE)):
            for a, b, w in segments:
                ww = max(1.0, w * plan.scale + extra)
                plan.draw.line([a, b], fill=c, width=int(round(ww)))
                for x, y in (a, b):
                    plan.draw.ellipse([x - ww / 2, y - ww / 2, x + ww / 2, y + ww / 2], fill=c)
        return plan.finish()
    return picture


for _v in ("displacement", "extrusion", "combined"):
    _add("fuzzy_mode_" + _v, _fuzzy_mode(_v))

