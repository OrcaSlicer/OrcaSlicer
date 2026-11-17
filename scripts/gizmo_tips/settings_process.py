"""Pictures for the tooltips of the Process settings (src/slic3r/GUI/SettingTipsProcess.cpp).

Most are drawn from real toolpaths: settings_process_slice.py slices small test models with OrcaSlicer
once per value of a setting and keeps the extrusions, so a pattern, a seam or a wall order here is the
one the slicer prints. Two ways of drawing them:

- a plan: one layer seen from straight above, every extrusion at its real width. The setting's lines are
  in ACCENT, the walls around them grey, and the layers just beneath fade out under them, so a pattern
  that changes from layer to layer shows how. Patterns and wall orders read best like this.
- a model: the extrusions of many layers as round beads, in the plater's view of a dark bed (the same
  camera and light as the ray-marched sets), with what the setting places in ACCENT. Seams, supports,
  brims, ironing and fuzzy skin read best like this.

The values of one setting share one model and one framing, so whatever differs between two of its
pictures is the setting.
"""

import gzip
import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, GIF_SIZE, LIGHT, PART, PART2, SIZE, SS, Camera, bed, phase, render, smooth
from .settings_process_slice import DATA, SPARSE, SPARSE_Z, SURFACE

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case, value):
    """The sliced layers of one value of a setting, from settings_process_toolpaths.json.gz."""
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data[case][value]


def _layer(layers, z):
    return min(layers, key=lambda layer: abs(layer["z"] - z))


def _paths(layer, test):
    """The paths of a layer whose type passes `test`, with their points as an (n, 2) array in mm."""
    return [dict(p, pts=np.asarray(p["p"], float).reshape(-1, 2)) for p in layer["paths"] if test(p["t"])]


def _rgb(c, k=1.0):
    return tuple(int(min(255.0, v * k * 255.0 + 0.5)) for v in c)


LINE = _rgb(ACCENT, 1.25)  # the setting's own extrusions; ACCENT lifted a little, as thin lines read darker
WALL = _rgb(PART)
UNDER = PART2  # the layers beneath, fading
EDGE = _rgb(BED, 0.5)  # the dark rim around each bead, which keeps neighbouring lines apart


def _partial(paths, f):
    """The first fraction f of `paths` by length, in the order they are printed: a layer being printed."""
    lengths = [np.linalg.norm(np.diff(p["pts"], axis=0), axis=1) for p in paths]
    left = f * sum(float(seg.sum()) for seg in lengths)
    out = []
    for p, seg in zip(paths, lengths):
        if left <= 1e-9:
            break
        if seg.sum() <= left:
            out.append(p)
            left -= seg.sum()
            continue
        cum = np.concatenate([[0.0], np.cumsum(seg)])
        i = min(max(int(np.searchsorted(cum, left)) - 1, 0), len(seg) - 1)
        t = (left - cum[i]) / max(seg[i], 1e-9)
        pts = np.vstack([p["pts"][: i + 1], p["pts"][i] + (p["pts"][i + 1] - p["pts"][i]) * t])
        out.append(dict(p, pts=pts))
        break
    return out


# --- plans: a layer from above --------------------------------------------------------------------


class Plan:
    """A layer seen from above, `span` mm across, centred on the model."""

    def __init__(self, span, size, centre=(0.0, 0.0)):
        self.size = size
        self.px = size * SS * 2  # drawn at four times the size: thin lines need the antialiasing
        self.scale = self.px / span
        self.centre = np.asarray(centre, float)
        self.image = Image.new("RGB", (self.px, self.px), _rgb(BED, 0.75))
        self.draw = ImageDraw.Draw(self.image)

    def xy(self, pts):
        q = (np.asarray(pts, float) - self.centre) * self.scale
        return [(float(x) + self.px / 2, self.px / 2 - float(y)) for x, y in q]

    def beads(self, paths, colour, edge=EDGE, thin=1.0):
        """Extrusions at their width (times `thin`), each with a dark rim if `edge` is given."""
        for p in paths:
            xy = self.xy(p["pts"])
            w = p["w"] * thin * self.scale
            layers = ((w + 1.5 * SS * 2, edge), (w, colour)) if edge else ((w, colour),)
            for width, c in layers:
                self.draw.line(xy, fill=c, width=max(1, int(round(width))), joint="curve")
                r = width / 2.0
                for x, y in (xy[0], xy[-1]):
                    self.draw.ellipse([x - r, y - r, x + r, y + r], fill=c)

    def dot(self, pt, radius, colour, outline=EDGE):
        (x, y), = self.xy([pt])
        r = radius * SS * 2
        self.draw.ellipse([x - r, y - r, x + r, y + r], fill=colour, outline=outline, width=SS * 2)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _under(plan, layers, below, test):
    """The `below` layers under the top one, fading as they go down."""
    for j in range(below, 0, -1):
        plan.beads(_paths(layers[-1 - j], test), _rgb(UNDER, 0.62 - 0.035 * j), edge=None)


# --- models: many layers in the plater's view ----------------------------------------------------

AROUND = 20  # samples around a bead's section


def _bead_points(items, scale, px_per_unit):
    """Points on the surface of every bead, with their normals and colours. items: (z, height, path, colour),
    the bead lying from z - height to z."""
    P, N, C = [], [], []
    th = np.linspace(0.0, 2.0 * math.pi, AROUND, endpoint=False)
    ct, st = np.cos(th), np.sin(th)
    step = 0.6 / px_per_unit
    for z, h, p, colour in items:
        pts = p["pts"] * scale
        a = pts[:-1]
        d = pts[1:] - a
        length = np.linalg.norm(d, axis=1)
        ok = length > 1e-9
        a, d, length = a[ok], d[ok] / length[ok, None], length[ok]
        if not len(a):
            continue
        w, hh = p["w"] * scale * 0.5, h * scale * 0.5
        n = np.stack([-d[:, 1], d[:, 0]], axis=1)
        count = np.maximum(1, np.ceil((length + w) / step).astype(int))
        seg = np.repeat(np.arange(len(a)), count)
        t = (np.arange(count.sum()) - np.repeat(np.cumsum(count) - count, count)) * step - 0.5 * w
        c = a[seg] + d[seg] * t[:, None]
        nn = n[seg]
        xy = c[:, None, :] + nn[:, None, :] * (w * ct)[None, :, None]
        zz = np.broadcast_to(z * scale - hh + hh * st, xy.shape[:2])
        P.append(np.concatenate([xy, zz[..., None]], axis=2).reshape(-1, 3))
        nrm = np.concatenate([nn[:, None, :] * (ct / w)[None, :, None],
                              np.broadcast_to((st / hh)[None, :, None], (len(c), AROUND, 1))], axis=2).reshape(-1, 3)
        N.append(nrm / np.linalg.norm(nrm, axis=1, keepdims=True))
        C.append(np.broadcast_to(np.asarray(colour, float), (len(nrm), 3)))
    return np.concatenate(P), np.concatenate(N), np.concatenate(C)


class Model:
    """Beads drawn into the plater's view: splatted surface points with a depth buffer, shaded like the
    ray-marched sets and outlined where the depth jumps. 1 mm is `scale` scene units."""

    def __init__(self, items, size, scale=0.1, **view):
        self.cam = cam = Camera(size, **view)
        self.size = size
        side = cam.px
        rgb = render(lambda p: bed(p), cam)
        P, N, C = _bead_points(items, scale, side / cam.extent)
        seen = N @ cam.c > -0.05
        P, N, C = P[seen], N[seen], C[seen]
        q = P - cam.target
        ix = np.floor((q @ cam.right / cam.extent + 0.5) * side).astype(int)
        iy = np.floor((0.5 - q @ cam.up / cam.extent) * side).astype(int)
        depth = q @ cam.c
        ok = (ix >= 0) & (ix < side) & (iy >= 0) & (iy < side)
        ix, iy, depth, N, C = ix[ok], iy[ok], depth[ok], N[ok], C[ok]
        order = np.argsort(depth)
        nearest = np.full(side * side, -1)
        nearest[iy[order] * side + ix[order]] = order  # the last one written, the nearest, stays
        hit = nearest >= 0
        sel = nearest[hit]
        lam = np.clip(N[sel] @ LIGHT, 0.0, 1.0)
        half = (LIGHT + cam.c) / np.linalg.norm(LIGHT + cam.c)
        spec = np.clip(N[sel] @ half, 0.0, 1.0) ** 24
        out = rgb.reshape(-1, 3).copy()
        out[hit] = C[sel] * (0.38 + 0.62 * lam)[:, None] + 0.10 * spec[:, None]
        zbuf = np.full(side * side, -np.inf)
        zbuf[hit] = depth[sel]
        out, zbuf = out.reshape(side, side, 3), zbuf.reshape(side, side)
        sil = np.zeros((side, side), bool)
        for dy, dx in ((0, 1), (1, 0)):
            a, b = zbuf[: side - dy, : side - dx], zbuf[dy:, dx:]
            with np.errstate(invalid="ignore"):
                jump = np.where(np.isfinite(a) & np.isfinite(b), np.abs(a - b) > 0.08, np.isfinite(a) != np.isfinite(b))
            sil[: side - dy, : side - dx] |= jump
            sil[dy:, dx:] |= jump
        out = np.where(sil[..., None], out * 0.45, out)
        self.scale = scale
        self.image = Image.fromarray((np.clip(out, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))
        self.draw = ImageDraw.Draw(self.image)

    def pt(self, x, y, z):
        return self.cam.project(np.array([x, y, z]) * self.scale)

    def dot(self, x, y, z, radius, colour, outline=None):
        px, py = self.pt(x, y, z)
        r = radius * SS
        self.draw.ellipse([px - r, py - r, px + r, py + r], fill=colour, outline=outline, width=SS)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _items(layers, test, colour, height=0.2):
    return [(layer["z"], height, p, colour) for layer in layers for p in _paths(layer, test)]


# --- sparse_infill_pattern: the layer under the top shells of a 30 mm cube ------------------------


def _sparse(value):
    def picture(k, n):
        layers = [layer for layer in _case("sparse_infill_pattern", value) if layer["z"] < SPARSE_Z + 0.05]
        plan = Plan(33.0, SIZE)
        _under(plan, layers, 8, lambda t: t == "Sparse infill")
        top = layers[-1]
        plan.beads(_paths(top, lambda t: "wall" in t), WALL)
        plan.beads(_paths(top, lambda t: t == "Sparse infill"), LINE)
        return plan.finish()
    return picture


# --- top_surface_pattern (and bottom, internal solid): an L-shaped top surface being printed ------
#
# The notch's floor, finished a few layers earlier, sits dimmed in the back right; the L above it is
# printed in order, so a monotonic pattern can be told from one that fills region by region.

SURFACE_STEPS, SURFACE_HOLD = 15, 5


def _surface(value):
    def picture(k, n):
        layers = _case("top_surface_pattern", value)
        low, high = _layer(layers, 2.0), _layer(layers, 4.0)
        plan = Plan(17.5, GIF_SIZE)
        plan.beads(_paths(low, lambda t: "wall" in t), _rgb(PART, 0.75))
        plan.beads(_paths(low, lambda t: t == "Top surface"), _rgb(ACCENT, 0.9), thin=0.6)
        plan.beads(_paths(high, lambda t: "wall" in t), WALL)
        f = smooth(phase(k, 0, SURFACE_STEPS)) if k < SURFACE_STEPS else 1.0
        plan.beads(_partial(_paths(high, lambda t: t == "Top surface"), f), LINE, thin=0.6)
        return plan.finish()
    return picture


# --- seam_position: where each layer's outer wall starts, on a round tube -------------------------
#
# Only the outer walls are drawn, and from higher than the plater's view, so the inside of the back of
# the tube is in sight too: Back and Aligned back put the seam there.

SEAM_VIEW = dict(target=(0.0, 0.0, 0.55), extent=2.3, el=50.0)


def _seam(value):
    def picture(k, n):
        layers = _case("seam_position", value)
        model = Model(_items(layers, lambda t: t == "Outer wall", PART), SIZE, **SEAM_VIEW)
        front = model.cam.c[:2]
        for layer in layers:
            x, y = layer["paths"][0]["p"][:2]
            if x * front[0] + y * front[1] < 0.0:  # on the back, it is seen from inside
                r = math.hypot(x, y)
                x, y = x * (r - 0.5) / r, y * (r - 0.5) / r
            model.dot(x, y, layer["z"] - 0.1, 2.6, _rgb(ACCENT, 1.3))
        return model.finish()
    return picture


# --- wall_sequence and wall_direction: one layer's walls being printed ----------------------------

WALL_STEPS, WALL_HOLD = 16, 5


def _walls(case, value):
    def picture(k, n):
        layer = _case(case, value)[0]
        walls = _paths(layer, lambda t: "wall" in t)
        plan = Plan(16.5, GIF_SIZE)
        f = smooth(phase(k, 0, WALL_STEPS)) if k < WALL_STEPS else 1.0
        done = _partial(walls, f)
        for p in done:
            plan.beads([p], LINE if p["t"] == "Outer wall" or case == "wall_direction" else WALL)
        if k < WALL_STEPS and done:
            plan.dot(done[-1]["pts"][-1], 3.2, (235, 242, 242))  # the nozzle
        return plan.finish()
    return picture


# --- wall_generator: a wedge narrowing to nothing -------------------------------------------------


def _wall_generator(value):
    def picture(k, n):
        layer = _case("wall_generator", value)[0]
        plan = Plan(12.0, SIZE, centre=(2.0, 0.0))  # the narrow half, where the two differ
        plan.beads(_paths(layer, lambda t: "wall" not in t), WALL)
        plan.beads(_paths(layer, lambda t: "wall" in t), LINE)
        return plan.finish()
    return picture


# --- supports: a mushroom on a thin stem ---------------------------------------------------------

SUPPORT_VIEW = dict(target=(0.0, 0.0, 0.85), extent=2.75, el=20.0)


def _support_style(value):
    def picture(k, n):
        layers = _case("support_style", value)
        items = _items(layers, lambda t: t.startswith("Support"), ACCENT * 1.15)
        items += _items(layers, lambda t: not t.startswith("Support"), PART)
        return Model(items, SIZE, **SUPPORT_VIEW).finish()
    return picture


def _support_plan(case, value):
    """A layer of the support under the cap, from above, over the support layer beneath it, fading: the
    base around the stem, or the interface the cap rests on."""
    def picture(k, n):
        layers = _case(case, value)
        support = [layer for layer in layers if any(p["t"].startswith("Support") for p in layer["paths"])]
        plan = Plan(21.0, SIZE)
        plan.beads(_paths(support[-2], lambda t: t.startswith("Support")), _rgb(UNDER, 0.6), edge=None)
        plan.beads(_paths(layers[-1], lambda t: not t.startswith("Support")), WALL)
        plan.beads(_paths(support[-1], lambda t: t.startswith("Support")), LINE)
        return plan.finish()
    return picture


# --- brim_type: a tube's first layer, from above enough to see into the hole ---------------------

BRIM_VIEW = dict(target=(0.0, 0.0, 0.15), extent=2.3, el=52.0)


def _brim(value):
    def picture(k, n):
        layers = _case("brim_type", value)
        items = _items(layers, lambda t: t == "Brim", ACCENT * 1.15)
        items += _items(layers, lambda t: t != "Brim", PART)
        return Model(items, SIZE, **BRIM_VIEW).finish()
    return picture


# --- ironing_type: two steps, cut open at the front so the layers inside show --------------------


def _clip(paths, normal, offset):
    """The parts of `paths` on the side of the line normal . p = offset that normal points away from."""
    out = []
    normal = np.asarray(normal, float)
    for p in paths:
        pts = p["pts"]
        s = pts @ normal - offset
        run = []
        for i in range(len(pts)):
            if s[i] <= 0.0:
                if not run and i > 0 and s[i - 1] > 0.0:
                    t = s[i - 1] / (s[i - 1] - s[i])
                    run.append(pts[i - 1] + (pts[i] - pts[i - 1]) * t)
                run.append(pts[i])
            elif run:
                t = s[i - 1] / (s[i - 1] - s[i])
                run.append(pts[i - 1] + (pts[i] - pts[i - 1]) * t)
                out.append(dict(p, pts=np.asarray(run)))
                run = []
        if len(run) > 1:
            out.append(dict(p, pts=np.asarray(run)))
    return out


IRONING_VIEW = dict(target=(0.0, 0.0, 0.3), extent=2.25, el=34.0)


def _ironing(value):
    def picture(k, n):
        items = []
        for layer in _case("ironing_type", value):
            for colour, test in ((ACCENT * 1.2, lambda t: t == "Ironing"), (PART, lambda t: t != "Ironing")):
                for p in _clip(_paths(layer, test), (0.0, -1.0), 1.0):
                    items.append((layer["z"] + (0.05 if colour is not PART else 0.0), 0.2, p, colour))
        return Model(items, SIZE, **IRONING_VIEW).finish()
    return picture


# --- fuzzy skin: a tube; the walls that came out fuzzy are in ACCENT -----------------------------


def _fuzzy_items(layers, every_wall=False):
    """Walls coloured by whether the slicer roughened them: their distance from the tube's axis wobbles.
    The inner walls are drawn too, behind the outer ones, so a gap the noise opens shows no hole."""
    items = []
    for layer in layers:
        for p in _paths(layer, lambda t: True):
            r = np.hypot(p["pts"][:, 0], p["pts"][:, 1])
            fuzzy = "wall" in p["t"] and (every_wall or (p["t"] == "Outer wall" and np.std(r) > 0.04))
            items.append((layer["z"], 0.2, p, ACCENT * 1.15 if fuzzy else PART))
    return items


FUZZY_VIEW = dict(target=(0.0, 0.0, 0.4), extent=2.15, el=45.0)


def _fuzzy(case, value):
    def picture(k, n):
        layers = _case(case, value)
        view = FUZZY_VIEW if case == "fuzzy_skin" else dict(target=(0.0, 0.0, 0.4), extent=1.85, el=30.0)
        # The noise types are all drawn on a fuzzy contour. Some leave stretches of the outer wall nearly
        # smooth, and Voronoi pulls patches of it in behind the inner wall, so there every wall is in
        # ACCENT: the surface seen is the setting's.
        return Model(_fuzzy_items(layers, case != "fuzzy_skin"), SIZE, **view).finish()
    return picture


# --- make_overhang_printable, draft_shield --------------------------------------------------------


def _overhang_printable(value):
    def picture(k, n):
        items = []
        for layer in _case("make_overhang_printable", value):
            for p in _paths(layer, lambda t: True):
                grown = layer["z"] < 8.0 and np.hypot(p["pts"][:, 0], p["pts"][:, 1]).max() > 3.4
                items.append((layer["z"], 0.2, p, ACCENT * 1.15 if grown else PART))
        return Model(items, SIZE, target=(0.0, 0.0, 0.55), extent=2.2, el=18.0).finish()
    return picture


def _draft_shield(value):
    def picture(k, n):
        layers = _case("draft_shield", value)
        items = _items(layers, lambda t: t == "Skirt", ACCENT * 1.15) + _items(layers, lambda t: t != "Skirt", PART)
        return Model(items, SIZE, target=(0.0, 0.0, 0.4), extent=2.2, el=35.0).finish()
    return picture


# --- thick_bridges: the deck's first layer over the gap, from above -------------------------------


def _thick_bridges(value):
    def picture(k, n):
        layer = _layer(_case("thick_bridges", value), 6.0)
        plan = Plan(23.0, SIZE)
        plan.beads(_paths(layer, lambda t: t != "Bridge"), WALL)
        plan.beads(_paths(layer, lambda t: t == "Bridge"), LINE)
        return plan.finish()
    return picture


# --- counterbore_hole_bridging: the layers over the wide bore -------------------------------------


def _counterbore(value):
    """The first layer over the bore, from above, with the bore's edge fading beneath it."""
    def picture(k, n):
        layers = _case("counterbore_hole_bridging", value)
        plan = Plan(20.0, SIZE)
        plan.beads(_paths(_layer(layers, 3.8), lambda t: "wall" in t), _rgb(UNDER, 0.6), edge=None)
        over = _layer(layers, 4.0)
        plan.beads(_paths(over, lambda t: t == "Sparse infill"), _rgb(PART, 0.6))
        plan.beads(_paths(over, lambda t: "wall" in t and t != "Overhang wall"), WALL)
        plan.beads(_paths(over, lambda t: t in ("Bridge", "Overhang wall", "Internal solid infill")), LINE)
        return plan.finish()
    return picture


# --- extra_perimeters_on_overhangs: the cap's first layer, hanging over nothing --------------------


def _extra_perimeters(value):
    def picture(k, n):
        layers = _case("extra_perimeters_on_overhangs", value)
        plan = Plan(18.0, SIZE)
        plan.beads(_paths(_layer(layers, 8.0), lambda t: "wall" in t), _rgb(UNDER, 0.6), edge=None)
        over = _layer(layers, 8.2)
        plan.beads(_paths(over, lambda t: t not in ("Bridge", "Overhang wall")), WALL)
        plan.beads(_paths(over, lambda t: t in ("Bridge", "Overhang wall")), LINE, thin=0.6)
        return plan.finish()
    return picture


# --- ironing_pattern: a corner of an ironed top, close up: the ironing's lines over the flat top ---


def _ironing_pattern(value):
    def picture(k, n):
        layer = _case("ironing_pattern", value)[0]
        plan = Plan(4.5, SIZE, centre=(-6.0, -6.0))  # the lines are a tenth of a millimetre apart
        plan.beads(_paths(layer, lambda t: t != "Ironing"), _rgb(PART, 0.7), edge=None)
        plan.beads(_paths(layer, lambda t: t == "Ironing"), LINE, edge=None, thin=0.2)
        return plan.finish()
    return picture


# --- the table: picture name -> frame count, and the functions -----------------------------------

FRAMES = {}


def _add(name, frames, fn):
    FRAMES[name] = frames
    globals()[name] = fn


def _slug(value):
    return value.replace("-", "_").replace("/", "_").replace(" ", "_")


for _v in SPARSE:
    _add("sparse_" + _slug(_v), 0, _sparse(_v))
for _v in SURFACE:
    _add("surface_" + _slug(_v), SURFACE_STEPS + SURFACE_HOLD, _surface(_v))
for _v in ("nearest", "aligned", "aligned_back", "back", "random"):
    _add("seam_" + _v, 0, _seam(_v))
for _v, _name in (("inner wall/outer wall", "inner_outer"), ("outer wall/inner wall", "outer_inner"),
                  ("inner-outer-inner wall", "inner_outer_inner")):
    _add("wall_sequence_" + _name, WALL_STEPS + WALL_HOLD, _walls("wall_sequence", _v))
for _v in ("ccw", "cw"):
    _add("wall_direction_" + _v, WALL_STEPS + WALL_HOLD, _walls("wall_direction", _v))

_add("wall_generator_classic", 0, _wall_generator("classic"))
_add("wall_generator_arachne", 0, _wall_generator("arachne"))
# Tree Strong differs from Tree Slim only in how branches gather on their way down, which on this model
# slices to the same G-code: the two share support_style_tree_slim.png.
for _v in ("grid", "snug", "organic", "tree_slim", "tree_hybrid"):
    _add("support_style_" + _v, 0, _support_style(_v))
for _v in ("rectilinear", "rectilinear-grid", "honeycomb", "lightning", "hollow"):
    _add("support_base_" + _slug(_v), 0, _support_plan("support_base_pattern", _v))
for _v in ("auto", "rectilinear", "concentric", "spiralinset", "rectilinear_interlaced", "grid"):
    _add("support_interface_" + _v, 0, _support_plan("support_interface_pattern", _v))
for _v in ("outer_only", "inner_only", "outer_and_inner", "no_brim"):
    _add("brim_" + _v, 0, _brim(_v))
for _v, _name in (("no ironing", "none"), ("top", "top"), ("topmost", "topmost"), ("solid", "solid")):
    _add("ironing_" + _name, 0, _ironing(_v))
for _v in ("external", "hole", "all", "disabled_fuzzy"):
    _add("fuzzy_skin_" + _v, 0, _fuzzy("fuzzy_skin", _v))
for _v in ("classic", "perlin", "billow", "ridgedmulti", "voronoi", "ripple"):
    _add("fuzzy_noise_" + _v, 0, _fuzzy("fuzzy_skin_noise_type", _v))
for _v, _name in (("0", "off"), ("1", "on")):
    _add("overhang_printable_" + _name, 0, _overhang_printable(_v))
    _add("thick_bridges_" + _name, 0, _thick_bridges(_v))
for _v in ("disabled", "enabled"):
    _add("draft_shield_" + _v, 0, _draft_shield(_v))
for _v in ("none", "partiallybridge", "sacrificiallayer"):
    _add("counterbore_" + _v, 0, _counterbore(_v))
for _v, _name in (("0", "off"), ("1", "on")):
    _add("extra_perimeters_on_overhangs_" + _name, 0, _extra_perimeters(_v))
for _v in ("rectilinear", "concentric"):
    _add("ironing_pattern_" + _v, 0, _ironing_pattern(_v))
