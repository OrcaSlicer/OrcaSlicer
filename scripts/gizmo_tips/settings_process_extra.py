"""Pictures for the tooltips of the Process settings the other settings_process* modules leave out, registered
in the SettingTipsProcess{Quality,Strength,Support,Others}.cpp file of the page each setting is on.

Most are drawn from toolpaths OrcaSlicer sliced for each value (settings_process_extra_slice.py), the ways
settings_process.py draws its own: a plan (one layer from straight above), a model (many layers as beads in
the plater's view) or a section (the plane y = 0 from the front). What the setting places or changes is in
ACCENT, the rest grey. The two values of a comparison share one model and one framing, so whatever differs
between the two pictures is the setting.

A few settings act on something no G-code line of a small test print shows - a temperature over time, the
colours painted into a part, a belt printer's brim - and are drawn as plain diagrams in the same palette.
"""

import gzip
import json
import math

import numpy as np
from PIL import Image, ImageDraw

from .render import (ACCENT, BED, GIF_SIZE, GRID, M_ACCENT, M_PART, PART, PART2, SIZE, SS, Camera, Overlay, bed, phase,
                     render, sd_box, smooth, union)
from .settings_process import EDGE, LINE, UNDER, WALL, Model, Plan, _clip, _partial, _paths, _rgb
from .settings_process_extra_slice import DATA
from .settings_process_others import Chart, _fit
from .settings_process_support import Section

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
ACC_RGB = _rgb(ACCENT, 1.25)  # ACCENT for what is drawn flat
LIGHT_LINE = (226, 232, 232)  # a travel, the nozzle

FRAMES = {}


def _add(name, fn, frames=0):
    FRAMES[name] = frames
    globals()[name] = fn


def _items(layers, colour_of):
    """Beads of every path, coloured by colour_of(path, layer) (None leaves the path out)."""
    out = []
    for layer in layers:
        for p in _paths(layer, lambda t: True):
            c = colour_of(p, layer)
            if c is not None:
                out.append((layer["z"], p.get("h", 0.2), p, c))
    return out


def _plan(case, value, z, span, accent, grey=lambda t: True, centre=(0.0, 0.0), under=None, thin=1.0):
    """One layer from above: `accent(type)` in ACCENT over the rest (`grey(type)`) in grey, and the layer at
    `under` (a height) dimmed beneath them."""
    def picture(k, n):
        layers = _case(case, value)
        plan = Plan(span, SIZE, centre=centre)
        if under is not None:
            plan.beads(_paths(_layer(layers, under), lambda t: True), _rgb(UNDER, 0.55), edge=None)
        layer = _layer(layers, z)
        plan.beads(_paths(layer, lambda t: grey(t) and not accent(t)), WALL)
        plan.beads(_paths(layer, accent), LINE, thin=thin)
        return plan.finish()
    return picture


def _pair(prefix, values, draw):
    """One picture per value of a comparison: prefix_<value>, drawn by draw(value)."""
    for v in values:
        _add("%s_%s" % (prefix, v.replace(".", "_").replace("%", "")), draw(v))


def _is_wall(t):
    return "wall" in t


# --- Quality: bridges, precision, overhangs, walls ------------------------------------------------


def _internal_bridges(value):
    """A block with a gently sloping top over sparse infill, from above: every internal bridge the slicer
    laid under the top's solid layers, in ACCENT, over a layer of the sparse infill."""
    def picture(k, n):
        layers = _case("internal_bridges", value)
        plan = Plan(23.0, SIZE)
        plan.beads(_paths(_layer(layers, 3.2), lambda t: t == "Sparse infill"), _rgb(UNDER, 0.6), edge=None)
        plan.beads(_paths(_layer(layers, 3.0), _is_wall), WALL)
        for layer in layers:
            plan.beads(_paths(layer, lambda t: t == "Internal Bridge"), LINE)
        return plan.finish()
    return picture


for _v in ("disabled", "limited", "nofilter"):
    _add("internal_bridge_filter_" + _v, _internal_bridges(_v))

# Elephant foot layers: the second layer's solid infill, over the first layer.
_pair("ef_density", ("100%", "50%"), lambda v: _plan("ef_density", "density_" + v, 0.4, 18.5,
                                                     lambda t: t == "Internal solid infill", under=0.2))


def _ironing_fixed(value):
    """Two steps whose tops are an odd number of layers apart, from above: the ironing (ACCENT) over each top
    follows that top's lines, or runs the same way on both."""
    def picture(k, n):
        layers = _case("ironing_fixed", value)
        plan = Plan(18.5, SIZE)
        for z in (3.0, 5.8):
            layer = _layer(layers, z)
            plan.beads(_paths(layer, lambda t: t == "Top surface"), _rgb(PART, 0.62), edge=None)
            plan.beads(_paths(layer, _is_wall), WALL)
            plan.beads(_paths(layer, lambda t: t == "Ironing"), LINE, edge=None, thin=0.5)
        return plan.finish()
    return picture


_pair("ironing_angle_fixed", ("0", "1"), lambda v: _ironing_fixed("fixed_" + v))


def _hole_size(value):
    """A block with a recess in its base, cut open through the recess: Make overhangs printable fills the
    recess with a cone from its roof down (ACCENT), unless the recess is small enough to keep."""
    def picture(k, n):
        items = []
        for layer in _case("hole_size", value):
            for p in _clip(_paths(layer, lambda t: True), (0.0, -1.0), 0.0):
                inside = layer["z"] < 4.0 and np.hypot(p["pts"][:, 0], p["pts"][:, 1]).min() < 2.9
                items.append((layer["z"], 0.2, p, ACC if inside else PART))
        return Model(items, SIZE, target=(0.0, 0.25, 0.3), extent=2.2, el=12.0, az=25.0).finish()
    return picture


_pair("overhang_hole_size", ("0", "50"), lambda v: _hole_size("hole_" + v))


# One wall threshold: the shelves beside the upper block. The walls (ACCENT) on a shelf too narrow to count as
# a top surface stay as many as below it.
_pair("one_wall_threshold", ("0", "5"), lambda v: _plan("one_wall_threshold", "thr_" + v, 3.0, 18.5, _is_wall,
                                                            grey=lambda t: t in ("Top surface",), under=2.8))


def _reverse(value):
    """The walls of a cap that flares out over its stem: the loops printed the other way round in ACCENT."""
    def picture(k, n):
        layers = _case("reverse", value)

        def turn(p):
            pts = p["pts"]
            return np.sign(np.sum(pts[:-1, 0] * pts[1:, 1] - pts[1:, 0] * pts[:-1, 1]))

        usual = np.sign(sum(turn(p) for layer in _case("reverse", "off") for p in _paths(layer, _is_wall)))
        items = [(layer["z"], 0.2, p, ACC if turn(p) == -usual else PART)
                 for layer in layers for p in _clip(_paths(layer, _is_wall), (0.0, -1.0), -0.5)
                 if len(p["pts"]) > 1]
        return Model(items, SIZE, target=(0.0, 0.3, 0.4), extent=2.6, el=12.0, az=20.0).finish()
    return picture


for _v in ("off", "on", "internal", "thr0"):
    _add("overhang_reverse_" + _v, _reverse(_v))

# Precise wall: a corner of the walls close up; the gap between the outer wall and the next one.
_pair("precise_outer_wall", ("0", "1"), lambda v: _plan("precise_wall", "precise_" + v, 1.0, 4.0,
                                                        lambda t: t == "Outer wall", grey=_is_wall, centre=(-6.6, -6.6)))


def _precise_z(value):
    """The top of a block 3.1 mm tall, cut through and seen from the side: its layers, and the height of the
    model (a light line). Off, the last layer stops short of it or overshoots; on, the last few layers are
    made a little thicker (ACCENT) so the top lands on it."""
    def picture(k, n):
        layers = _case("precise_z", value)
        s = Section((0.0, 2.4), 3.6)
        beads = []
        for layer in layers:
            h = layer["paths"][0]["h"]
            colour = ACC_RGB if abs(h - 0.2) > 0.005 else WALL
            beads.append((-6.0, 6.0, layer["z"] - h, layer["z"], colour))
        s.beads(beads)
        y = s.xy(0.0, 3.1)[1]
        s.draw.line([(0, y), (s.px, y)], fill=LIGHT_LINE, width=SS * 4)
        return s.finish()
    return picture

_pair("precise_z_height", ("0", "1"), lambda v: _precise_z("z_" + v))

# Slice gap closing radius: a block with a slit 0.3 mm wide cut into it. A radius larger than half the slit
# closes it, and the walls (ACCENT) run straight past it.
_pair("slice_closing_radius", ("0.049", "0.2"), lambda v: _plan("closing", "radius_" + v, 1.0, 18.5, _is_wall))


UNSUPPORTED_STEPS, UNSUPPORTED_HOLD = 17, 5


def _unsupported_last(value):
    """The first layer of a cap over a thin stem being printed: its outer loops hang in the air (ACCENT).
    Printed first, they have nothing to hold them; last, they rest on the bridges."""
    def picture(k, n):
        layer = _case("unsupported_last", "last_" + value)[0]
        paths = _paths(layer, lambda t: True)
        plan = Plan(18.5, GIF_SIZE)
        f = smooth(phase(k, 0, UNSUPPORTED_STEPS)) if k < UNSUPPORTED_STEPS else 1.0
        done = _partial(paths, f)
        for p in done:
            plan.beads([p], LINE if p["t"] == "Overhang wall" else WALL, thin=0.8)
        if k < UNSUPPORTED_STEPS and done:
            plan.dot(done[-1]["pts"][-1], 3.2, LIGHT_LINE)
        return plan.finish()
    return picture


for _v in ("0", "1"):
    _add("unsupported_wall_last_" + _v, _unsupported_last(_v), UNSUPPORTED_STEPS + UNSUPPORTED_HOLD)


def _zaa(value):
    """The top layer (ACCENT, thinned) over the solid layer under it: with Z contouring they cross, or, not
    alternating, run the same way."""
    def picture(k, n):
        layers = _case("zaa", "alt_" + value)
        plan = Plan(9.0, SIZE, centre=(-3.6, -3.6))  # a corner, close up
        top, below = layers[-1], layers[-2]
        plan.beads(_paths(below, lambda t: not _is_wall(t)), _rgb(PART, 0.8), thin=0.6)
        plan.beads(_paths(top, _is_wall), WALL)
        plan.beads(_paths(top, lambda t: not _is_wall(t)), LINE, thin=0.4)
        return plan.finish()
    return picture


_pair("zaa_dont_alternate", ("0", "1"), _zaa)


# --- Strength: infill ------------------------------------------------------------------------------

# Anchor length: a corner of the infill close up, where its lines meet the walls.
_pair("infill_anchor", ("0", "400%"), lambda v: _plan("anchor", "anchor_" + v, 1.6, 9.0, lambda t: t == "Sparse infill",
                                                      centre=(-4.2, -4.2)))
# Lock depth: Locked Zag's skin and skeleton overlap by this much.
_pair("infill_lock_depth", ("0", "2"), lambda v: _plan("lock_depth", "lock_" + v, 3.0, 22.5, lambda t: t == "Sparse infill"))


def _solid_template(value):
    """Four solid layers of a block from above, each peeled back a strip further than the one under it, so
    the direction of every one shows: the top one in ACCENT. With a template each layer turns by the next
    angle in it."""
    def picture(k, n):
        layers = _case("solid_template", value)[-4:]
        plan = Plan(18.5, SIZE)
        for j, layer in enumerate(layers):
            cut = -8.0 + 4.0 * j  # this layer shows from x = cut on
            paths = _clip(_paths(layer, lambda t: True), (-1.0, 0.0), -cut)
            last = j == len(layers) - 1
            plan.beads([p for p in paths if not _is_wall(p["t"])], LINE if last else _rgb(PART, 0.55 + 0.12 * j),
                       thin=0.6)
        return plan.finish()
    return picture


_pair("solid_infill_rotate_template", ("none", "template"), _solid_template)


def _symmetric(value):
    """A part (left) and its mirror image (right), from above: the right one's infill runs the same way, or,
    with the setting on for it, mirrors the left one's."""
    def picture(k, n):
        plan = Plan(38.0, SIZE)
        for layer in (_case("symmetric_a", "off")[0], _case("symmetric_b", "sym_" + value)[0]):
            plan.beads(_paths(layer, _is_wall), WALL)
            plan.beads(_paths(layer, lambda t: t == "Sparse infill"), LINE)
        return plan.finish()
    return picture


_pair("symmetric_infill_y_axis", ("0", "1"), _symmetric)


# --- Support ---------------------------------------------------------------------------------------


def _independent(value):
    """The support's top under a cap, cut through: the gap to the part in ACCENT. Tied to the part's layers
    the support stops a whole layer short; with layers of its own it keeps the gap that was set."""
    def picture(k, n):
        layers = _case("independent", "ind_" + value)
        s = Section((5.0, 5.9), 3.0)
        outline = lambda z: [(-r, r) for r, z0, z1 in ((9, 0, 2), (2.5, 0, 7), (8, 6, 8)) if z0 <= z <= z1]
        support = s.cut(layers, lambda t: _rgb(PART2, 1.15))
        part = s.part(layers, outline)
        top = max(b[3] for b in support)
        lo, hi = min(b[0] for b in support), max(b[1] for b in support)
        above = min(b[2] for b in part if b[2] >= top - 1e-3 and b[1] > lo and b[0] < hi)  # the cap over the support
        (ax, ay), (bx, by) = s.xy(lo, above), s.xy(hi, top)
        s.draw.rectangle([ax, ay, bx, by], fill=ACC_RGB)
        s.beads(part)
        s.beads(support)
        return s.finish()
    return picture


_pair("independent_support_layers", ("0", "1"), _independent)


def _support_model(case, value, view):
    def picture(k, n):
        items = [(layer["z"], p["h"], p, ACC if p["t"].startswith("Support") else PART)
                 for layer in _case(case, value) for p in _paths(layer, lambda t: True)]
        return Model(items, SIZE, **view).finish()
    return picture




def _bridge_support(value):
    """Two piers and the deck across them, from above: the support's top layer under the bridge (ACCENT), over
    the piers' last layer. A bridge shorter than the maximum gets support only at its free edges."""
    def picture(k, n):
        layers = _case("bridge_length", "len_" + value)
        plan = Plan(21.0, SIZE)
        plan.beads(_paths(_layer(layers, 5.6), lambda t: not t.startswith("Support")), WALL)
        support = [layer for layer in layers if any(p["t"].startswith("Support") for p in layer["paths"])]
        plan.beads(_paths(support[-1], lambda t: t.startswith("Support")), LINE)
        return plan.finish()
    return picture


_pair("max_bridge_length", ("0", "10"), _bridge_support)
_pair("support_critical_only", ("0", "1"), lambda v: _support_model("critical", "crit_" + v,
                                                                     dict(target=(0.0, 0.45, 0.3), extent=3.0, el=22.0)))
_pair("support_interface_loop", ("0", "1"), lambda v: _plan("loop_interface", "loop_" + v, 7.8, 21.0,
                                                            lambda t: t == "Support interface", thin=0.8))


def _auto_brim(value):
    """A tree support's first layer (ACCENT): with Auto brim width each branch's foot gets a brim sized to
    how tall the branch is."""
    def picture(k, n):
        items = []
        for layer in _case("auto_brim", "auto_" + value):
            for p in _paths(layer, lambda t: True):
                colour = (ACC if layer["z"] < 0.25 else PART2 * 1.1) if p["t"].startswith("Support") else PART
                items.append((layer["z"], p["h"], p, colour))
        return Model(items, SIZE, target=(0.0, 0.0, 0.7), extent=3.1, el=32.0).finish()
    return picture


_pair("tree_support_auto_brim", ("0", "1"), _auto_brim)


# --- Others: brim, G-code output, special modes ----------------------------------------------------


def _first_layer(case, value, span, centre=(0.0, 0.0)):
    return _plan(case, value, 0.2, span, lambda t: t == "Brim", centre=centre)


_pair("brim_ears_detection", ("0", "1.5"), lambda v: _first_layer("ears_detect", "len_" + v, 26.0, centre=(0.5, 0.5)))


def _exclude(value):
    """Three parts' first layer from above, and the outline written for each (ACCENT), with its points."""
    def picture(k, n):
        data = _case("exclude", "tol_" + value)
        plan = Plan(27.0, SIZE, centre=(0.0, 0.5))
        plan.beads(_paths(data["layers"][0], lambda t: True), _rgb(PART, 0.8))
        for o in data["outlines"]:
            pts = np.asarray(o, float).reshape(-1, 2)
            xy = plan.xy(np.vstack([pts, pts[:1]]))
            plan.draw.line(xy, fill=LINE, width=int(2.2 * SS * 2), joint="curve")
            for p in pts:
                plan.dot(p, 1.7, LINE, outline=None)
        return plan.finish()
    return picture


_pair("exclude_object_tolerance", ("0", "1"), _exclude)


def _retraction(value):
    """A layer of Lightning infill from above, the travels between its branches (light lines) and every
    retraction (ACCENT dots): travels inside the infill can go without one."""
    def picture(k, n):
        layer = _case("infill_retraction", "reduce_" + value)[0]
        plan = Plan(20.0, SIZE)
        plan.beads(_paths(layer, _is_wall), WALL)
        plan.beads(_paths(layer, lambda t: not _is_wall(t)), _rgb(PART, 0.75))
        for t in layer["travel"]:
            pts = np.asarray(t, float).reshape(-1, 2)
            plan.draw.line(plan.xy(pts), fill=LIGHT_LINE, width=int(1.2 * SS * 2))
        for p in np.asarray(layer["retract"], float).reshape(-1, 2):
            plan.dot(p, 4.0, LINE)
        return plan.finish()
    return picture


_pair("reduce_infill_retraction", ("0", "1"), _retraction)


# --- Multimaterial: the prime tower -------------------------------------------------------------------


def _tower(case, value, ref, el=30.0):
    """The tower alone (ACCENT) beside the part, framed on the tower of `ref` so two values compare."""
    def picture(k, n):
        # The tower's own paths: the wipes that start over the part are left out.
        colour = lambda p, layer: (ACC if p["pts"][:, 0].min() > 0.5 else None) if p["t"] == "Prime tower" else PART
        view = _fit(_items(_case(case, ref), colour), el=el, margin=1.05)
        return Model(_items(_case(case, value), colour), SIZE, **view).finish()
    return picture


_pair("prime_volume", ("15", "90"), lambda v: _tower("prime_volume", "vol_" + v, "vol_90"))


def _priming(value):
    """The whole bed from above at the start of the print: the part and its tower in the middle, and the
    priming lines (ACCENT) laid along the front edge."""
    def picture(k, n):
        layers = _case("priming", "prime_" + value)
        plan = Plan(212.0, SIZE)
        (x0, y0), (x1, y1) = plan.xy([(-100.0, 100.0), (100.0, -100.0)])
        plan.draw.rectangle([x0, y0, x1, y1], fill=_rgb(GRID, 1.15), outline=_rgb(PART, 0.5), width=SS * 2)
        first = _layer(layers, 0.2)
        plan.beads(_paths(first, lambda t: True), WALL, edge=None, thin=4.0)
        for layer in layers:
            if layer["z"] < 0.0:
                plan.beads(_paths(layer, lambda t: True), LINE, edge=None, thin=5.0)
        return plan.finish()
    return picture


_pair("multi_material_priming", ("0", "1"), _priming)


# --- diagrams --------------------------------------------------------------------------------------


def _tower_temperature(features, cooldown):
    """The new filament's nozzle temperature through a filament change on a layer where two materials meet:
    the bands under the chart are the purge on the prime tower, then the part."""
    def picture(k, n):
        chart = Chart()
        chart.grid()
        base, hot, tower_end = 0.45, 0.85, 0.5
        pts = []
        for i in range(201):
            t = i / 200.0
            if not features:
                temp = base
            elif t < 0.08:
                temp = base + (hot - base) * smooth(t / 0.08)
            elif cooldown:  # back to the print temperature from the start of the tower
                temp = base + (hot - base) * math.exp(-(t - 0.08) / 0.12)
            elif t < tower_end:
                temp = hot
            else:  # cools down while the part is printed
                temp = base + (hot - base) * math.exp(-(t - tower_end) / 0.12)
            pts.append((t, temp))
        chart.line([(0.0, base), (1.0, base)], _rgb(PART, 0.45), 1.5)
        chart.line(pts, ACC_RGB)
        chart.band(0.0, tower_end, _rgb(PART2, 1.1))
        chart.band(tower_end, 1.0, _rgb(PART))
        return chart.finish()
    return picture


_add("tower_interface_off", _tower_temperature(False, False))
_add("tower_interface_on", _tower_temperature(True, False))
_add("tower_interface_cooldown", _tower_temperature(True, True))


def _canvas(size=SIZE):
    px = size * SS * 2
    img = Image.new("RGB", (px, px), _rgb(BED, 0.75))
    return img, ImageDraw.Draw(img), px


def _finish(img, size=SIZE):
    return np.asarray(img.resize((size, size), Image.LANCZOS))


def _sublayers(on):
    """A wall printed in a mixed filament, cut through and seen from the side: its layers alternate between the
    two filaments it mixes, or, split into sub-layers, alternate within each layer."""
    def picture(k, n):
        img, draw, px = _canvas()
        rows, x0, x1, top, bottom = 7, 0.14 * px, 0.86 * px, 0.14 * px, 0.86 * px
        h = (bottom - top) / rows
        colours = (ACC_RGB, _rgb(PART))
        for r in range(rows):
            y1 = bottom - r * h
            parts = ((y1 - h, y1, colours[r % 2]),) if not on else \
                ((y1 - h / 2, y1, colours[0]), (y1 - h, y1 - h / 2, colours[1]))
            for a, b, c in parts:
                e = 1.2 * SS * 2
                draw.rounded_rectangle([x0 - e, a - e + 1, x1 + e, b + e - 1], radius=(b - a) / 2 + e, fill=EDGE)
                draw.rounded_rectangle([x0, a + 1, x1, b - 1], radius=(b - a) / 2, fill=c)
        return _finish(img)
    return picture


_add("mixed_color_sublayer_off", _sublayers(False))
_add("mixed_color_sublayer_on", _sublayers(True))


def _segmented(width, depth=None):
    """A layer of a square part with its left face painted, from above (width) or cut through and seen from
    the side (depth): the painted colour (ACCENT) reaches in from the face as far as the painting carries
    it, or only `width` deep; with an interlocking depth, every other layer stops shorter."""
    def picture(k, n):
        img, draw, px = _canvas()
        a, b = 0.16 * px, 0.84 * px
        side = b - a
        if depth is None:  # from above
            draw.rectangle([a, a, b, b], fill=_rgb(PART))
            if width == 0:  # as far as the painting carries: to where the other faces are as near
                draw.polygon([(a, a), (a + side / 2, a + side / 2), (a, b)], fill=ACC_RGB)
            else:
                draw.rectangle([a, a, a + side * width, b], fill=ACC_RGB)
            draw.rectangle([a, a, b, b], outline=_rgb(BED, 0.5), width=SS * 2)
        else:  # from the side, layer by layer
            rows = 9
            h = side / rows
            for r in range(rows):
                y1 = b - r * h
                reach = width if (depth == 0 or r % 2 == 0) else depth
                for x0, x1, c in ((a, a + side * reach, ACC_RGB), (a + side * reach, b, _rgb(PART))):
                    draw.rectangle([x0, y1 - h + SS, x1, y1 - SS], fill=c)
            draw.rectangle([a, a, b, b], outline=_rgb(BED, 0.5), width=SS * 2)
        return _finish(img)
    return picture


_add("mmu_region_width_0", _segmented(0))
_add("mmu_region_width_3", _segmented(0.2))
_add("mmu_region_interlock_0", _segmented(0.3, 0))
_add("mmu_region_interlock_on", _segmented(0.3, 0.12))


def _belt_brim(leading, extra):
    """A part's footprint on a belt seen from above, the belt running towards the front: its brim (ACCENT),
    longer ahead of the part with a leading length, wider to the sides with an extra width."""
    def picture(k, n):
        img, draw, px = _canvas()
        u = px / 40.0  # 40 mm across
        draw.rectangle([0.08 * px, 0, 0.92 * px, px], fill=_rgb(GRID, 1.1))
        for i in range(-2, 12):  # the belt's ribs, across it
            y = (i * 4.0 + 2.0) * u
            draw.line([(0.08 * px, y), (0.92 * px, y)], fill=_rgb(GRID, 1.35), width=SS * 2)
        cx, cy = px / 2, 0.42 * px
        w, d, brim = 6.0, 5.0, 3.0  # half the footprint, and the brim, in mm
        bx, by_back, by_front = w + brim + extra, d + brim, d + brim + leading
        draw.rounded_rectangle([cx - bx * u, cy - by_back * u, cx + bx * u, cy + by_front * u], radius=1.5 * u, fill=ACC_RGB)
        draw.rectangle([cx - w * u, cy - d * u, cx + w * u, cy + d * u], fill=_rgb(PART), outline=_rgb(BED, 0.5), width=SS * 2)
        return _finish(img)
    return picture


_add("belt_brim_plain", _belt_brim(0.0, 0.0))
_add("belt_brim_leading", _belt_brim(8.0, 0.0))
_add("belt_brim_extra", _belt_brim(0.0, 4.0))


def _belt_tower(width):
    """A part on a belt and the purge prism beside it (ACCENT): a wider prism is lower, for the same purge."""
    def picture(k, n):
        w = 0.18 + 0.32 * width
        h = 0.95 * 0.34 / w
        part = lambda p: sd_box(p, (-0.55, 0.0, 0.32), (0.32, 0.32, 0.32), 0.02)
        tower = lambda p: sd_box(p, (0.25 + w, 0.0, h / 2), (w, 0.22, h / 2), 0.02)
        scene = lambda p: union(bed(p), (part(p), M_PART), (tower(p), M_ACCENT))
        cam = Camera(SIZE, target=(0.15, 0.0, 0.45), extent=2.9)
        return Overlay(render(scene, cam), cam).finish(SIZE)
    return picture


_add("belt_purge_tower_narrow", _belt_tower(0.0))
_add("belt_purge_tower_wide", _belt_tower(1.0))


def _timelapse(smooth_mode):
    """The bed from above when a layer is done: the nozzle (light) stays over the part for the snapshot, or,
    in smooth mode, travels off to the chute and wipes on the prime tower (ACCENT) when it comes back."""
    def picture(k, n):
        img, draw, px = _canvas()
        draw.rectangle([0.1 * px, 0.1 * px, 0.9 * px, 0.9 * px], fill=_rgb(GRID, 1.1))
        draw.ellipse([0.36 * px, 0.4 * px, 0.6 * px, 0.64 * px], fill=_rgb(PART), outline=_rgb(BED, 0.5), width=SS * 2)
        nozzle = (0.5 * px, 0.5 * px)
        if smooth_mode:
            draw.rectangle([0.66 * px, 0.3 * px, 0.8 * px, 0.44 * px], fill=ACC_RGB, outline=_rgb(BED, 0.5), width=SS * 2)
            chute = (0.95 * px, 0.06 * px)
            pts = [nozzle, (0.62 * px, 0.37 * px), chute]
            for (xa, ya), (xb, yb) in zip(pts[:-1], pts[1:]):  # dashed
                for s in np.arange(0.0, 1.0, 0.12):
                    e = min(s + 0.07, 1.0)
                    draw.line([(xa + (xb - xa) * s, ya + (yb - ya) * s), (xa + (xb - xa) * e, ya + (yb - ya) * e)],
                              fill=ACC_RGB, width=int(2.4 * SS * 2))
            nozzle = chute
        r = 0.035 * px
        draw.ellipse([nozzle[0] - r, nozzle[1] - r, nozzle[0] + r, nozzle[1] + r], fill=LIGHT_LINE,
                     outline=_rgb(BED, 0.5), width=SS * 2)
        return _finish(img)
    return picture


_add("timelapse_traditional", _timelapse(False))
_add("timelapse_smooth", _timelapse(True))
