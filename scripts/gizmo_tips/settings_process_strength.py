"""Pictures for the tooltips of the Process tab's Strength page (src/slic3r/GUI/SettingTipsProcessStrength.cpp).

Drawn from real toolpaths, as settings_process.py draws the earlier Process pictures:
settings_process_strength_slice.py slices small test models once per value of a setting and keeps the
extrusions. Three ways of drawing them:

- a plan: one layer from straight above (settings_process.Plan), for patterns, directions and densities.
- a section: the part cut through by a vertical plane and seen from the side, every extrusion the plane
  cuts drawn as the bead it is, at its width and layer height. Walls, shells, inserted solid layers and
  anything that changes from layer to layer read best like this, the way a printed part sawn in half does.
- a model: many layers as beads in the plater's view (settings_process.Model), for the lightning trees.

The values of one setting share one model and one framing, so whatever differs between two of its
pictures is the setting. ACCENT marks the extrusions the setting changes.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, GIF_SIZE, PART, SIZE, SS, phase, smooth
from .settings_process import EDGE, LINE, UNDER, WALL, Model, Plan, _clip, _items, _partial, _paths, _rgb
from .settings_process_strength_slice import SECTION_Y, load

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case, value):
    global _data
    if _data is None:
        _data = load()
    return _data[case][value]


def _at(layers, z):
    return min(layers, key=lambda layer: abs(layer["z"] - z))


DIM = _rgb(PART, 0.55)  # what is there but not the point: the sparse infill around a wall, say
SOLID = ("Top surface", "Bottom surface", "Internal solid infill", "Bridge", "Internal Bridge")


def _is_wall(t):
    return "wall" in t.lower()


# --- sections: the part cut through and seen from the side --------------------------------------


class Section:
    """The extrusions a vertical plane cuts, seen from the front: horizontal is x (or y when the plane is
    x = const), vertical is z. `span` mm across, centred on (h, z) = `centre`."""

    def __init__(self, span, size=SIZE, centre=(0.0, 0.0)):
        self.size = size
        self.px = size * SS * 2
        self.scale = self.px / span
        self.centre = np.asarray(centre, float)
        self.image = Image.new("RGB", (self.px, self.px), _rgb(BED, 0.75))
        self.draw = ImageDraw.Draw(self.image)
        # The bed: a band under z = 0.
        y0 = self._y(0.0)
        self.draw.rectangle([0, y0 + SS * 2, self.px, self.px], fill=_rgb(BED, 1.15))

    def _x(self, h):
        return (h - self.centre[0]) * self.scale + self.px / 2

    def _y(self, z):
        return self.px / 2 - (z - self.centre[1]) * self.scale

    def cut(self, layers, colour, axis="y"):
        """Draws the beads where the plane cuts the paths. colour(z, path) -> RGB, or None to leave a path
        out. A path crossing the plane at a slant is cut longer than it is wide, as a real section is."""
        a, b = (1, 0) if axis == "y" else (0, 1)
        beads = []
        for layer in layers:
            z = layer["z"]
            for p in layer["paths"]:
                c = colour(z, p)
                if c is None:
                    continue
                pts = np.asarray(p["p"], float).reshape(-1, 2)
                d = pts[1:] - pts[:-1]
                s0, s1 = pts[:-1, a] - SECTION_Y, pts[1:, a] - SECTION_Y
                for i in np.nonzero((s0 * s1 < 0.0) | ((s0 == 0.0) & (s1 != 0.0)))[0]:
                    t = s0[i] / (s0[i] - s1[i])
                    h = pts[i, b] + d[i, b] * t
                    length = math.hypot(*d[i])
                    sin = abs(d[i, a]) / length if length > 1e-9 else 1.0
                    w = min(p["w"] / max(sin, 1e-3), 3.0 * p["w"])
                    beads.append((z, p.get("h", 0.2), h, w, c))
        rim = SS
        for z, hh, h, w, c in beads:
            x0, x1 = self._x(h - w / 2), self._x(h + w / 2)
            y0, y1 = self._y(z), self._y(z - hh)
            r = min(x1 - x0, y1 - y0) / 2
            self.draw.rounded_rectangle([x0 - rim, y0 - rim, x1 + rim, y1 + rim], radius=r + rim, fill=EDGE)
        for z, hh, h, w, c in beads:
            x0, x1 = self._x(h - w / 2), self._x(h + w / 2)
            y0, y1 = self._y(z), self._y(z - hh)
            r = min(x1 - x0, y1 - y0) / 2
            self.draw.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=c)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


def _section(case, value, span, centre, colour, axis="y"):
    def picture(k, n):
        s = Section(span, SIZE, centre)
        s.cut(_case(case, value), colour, axis)
        return s.finish()
    return picture


# --- plans ------------------------------------------------------------------------------------------


def _plan(case, value, span, z, accent, centre=(0.0, 0.0), dim=lambda t: False, thin=1.0, under=0, under_test=None):
    """One layer from above: the extrusions `accent` picks in ACCENT, those `dim` picks darker, the rest
    grey, with `under` layers fading beneath it."""
    def picture(k, n):
        layers = sorted(_case(case, value), key=lambda layer: layer["z"])
        top = _at(layers, z) if z is not None else layers[-1]
        plan = Plan(span, SIZE, centre)
        if under:
            below = [layer for layer in layers if layer["z"] < top["z"] - 1e-3][-under:]
            for j, layer in enumerate(below):
                fade = 0.35 + 0.25 * (j + 1) / len(below)
                plan.beads(_paths(layer, under_test or accent), _rgb(UNDER, fade), edge=None)
        rest = _paths(top, lambda t: not accent(t))
        plan.beads([p for p in rest if not dim(p["t"])], WALL)
        plan.beads([p for p in rest if dim(p["t"])], DIM)
        plan.beads(_paths(top, accent), LINE, thin=thin)
        return plan.finish()
    return picture


def _fill_order(value):
    """The top surface being printed, ring by ring, then held."""
    def picture(k, n):
        layer = _case("fill_order", value)[0]
        plan = Plan(15.0, GIF_SIZE)
        plan.beads(_paths(layer, _is_wall), WALL)
        f = smooth(phase(k, 0, ORDER_STEPS)) if k < ORDER_STEPS else 1.0
        done = _partial(_paths(layer, lambda t: t == "Top surface"), f)
        plan.beads(done, LINE)
        if k < ORDER_STEPS and done:
            plan.dot(done[-1]["pts"][-1], 3.2, (235, 242, 242))  # the nozzle
        return plan.finish()
    return picture


ORDER_STEPS, ORDER_HOLD = 16, 5


def _lightning(value):
    def picture(k, n):
        layers = _case("lightning", value)
        items = _items(layers, lambda t: t == "Sparse infill", ACCENT * 1.15)
        items += _items(layers, lambda t: t == "Outer wall", PART)
        return Model(items, SIZE, scale=0.1, target=(0.0, 0.0, 0.55), extent=2.9, el=24.0).finish()
    return picture


def _gap_fill(value):
    """One point of the star from above, twice: its top layer over a layer from the middle. The point is
    turned to run to the right; the gap fill is in ACCENT."""
    def picture(k, n):
        layers = _case("gap_fill", value)
        a = math.radians(-162.0)  # the point at 162 degrees, where the slicer leaves gaps on both layers
        turn = np.array([[math.cos(a), math.sin(a)], [-math.sin(a), math.cos(a)]])
        plan = Plan(8.0, SIZE, centre=(5.9, 0.0))
        for z, shift in ((1.2, 2.15), (0.8, -2.15)):
            paths = [dict(p, pts=p["pts"] @ turn + (0.0, shift)) for p in _paths(_at(layers, z), lambda t: True)]
            paths = _clip(paths, (-1.0, 0.0), -2.9)
            plan.beads([p for p in paths if _is_wall(p["t"])], WALL)
            plan.beads([p for p in paths if not _is_wall(p["t"]) and p["t"] != "Gap infill"], DIM, thin=0.8)
            plan.beads([p for p in paths if p["t"] == "Gap infill"], LINE)
        return plan.finish()
    return picture


# --- the table ---------------------------------------------------------------------------------------

FRAMES = {}


def _add(name, frames, fn):
    FRAMES[name] = frames
    globals()[name] = fn


def _slug(value):
    return value.replace("-", "_").replace("/", "_").replace(" ", "_").replace("%", "").replace("#", "_").replace(".", "_")


SPARSE = lambda t: t == "Sparse infill"

# Walls.
for _v in ("1", "3", "5"):
    _add("wall_loops_" + _v, 0, _plan("wall_loops", _v, 15.5, 1.0, _is_wall, dim=lambda t: True))
def _extra_wall_colour(z, p):
    # The sparse infill's anchors run along the inner wall and would be cut as a third wall: left out.
    return LINE if _is_wall(p["t"]) else (None if p["t"] == "Sparse infill" else DIM)


for _v, _n in (("0", "off"), ("1", "on")):
    _add("alternate_extra_wall_" + _n, 0,
         _section("alternate_extra_wall", _v, 4.6, (-6.1, 2.0), _extra_wall_colour))
    _add("detect_thin_wall_" + _n, 0, _plan("detect_thin_wall", _v, 14.0, 1.0, lambda t: True))

# Top and bottom shells, cut through: the solid layers at the top, or at the bottom, in ACCENT.
for _v in ("2", "5"):
    _add("top_shell_" + _v, 0, _section("shells", _v, 11.0, (0.0, 3.6),
                                        lambda z, p: LINE if p["t"] in SOLID and z > 3.5 else (WALL if _is_wall(p["t"]) else DIM)))
    _add("bottom_shell_" + _v, 0, _section("shells", _v, 11.0, (0.0, 3.6),
                                           lambda z, p: LINE if p["t"] in SOLID and z < 3.5 else (WALL if _is_wall(p["t"]) else DIM)))


def _penetration(top, count):
    """The layers a colour painted on the top (or bottom) surface reaches, in ACCENT."""
    def colour(z, p):
        if _is_wall(p["t"]):
            return WALL
        reached = z > 7.0 - 0.2 * count + 1e-3 if top else z < 0.2 * count + 1e-3
        return LINE if reached else DIM
    return colour


for _n in (2, 8):
    _add("top_paint_penetration_%d" % _n, 0, _section("shells", "5", 11.0, (0.0, 3.6), _penetration(True, _n)))
    _add("bottom_paint_penetration_%d" % _n, 0, _section("shells", "5", 11.0, (0.0, 3.6), _penetration(False, _n)))

for _v in ("100", "50", "25"):
    _add("top_surface_density_" + _v, 0, _plan("surface_density", _v, 15.5, 2.0, lambda t: t == "Top surface"))
    _add("bottom_surface_density_" + _v, 0, _plan("surface_density", _v, 15.5, 0.2, lambda t: t == "Bottom surface"))
for _v in ("default", "outward", "inward"):
    _add("surface_fill_order_" + _v, ORDER_STEPS + ORDER_HOLD, _fill_order(_v))

# Directions, at 0 and 30 degrees.
for _v in ("0", "30"):
    _add("infill_direction_" + _v, 0, _plan("direction", _v, 15.5, 1.6, SPARSE))
    _add("solid_infill_direction_" + _v, 0, _plan("direction", _v, 15.5, 2.6, lambda t: t == "Internal solid infill", thin=0.55))
    _add("top_layer_direction_" + _v, 0, _plan("direction", _v, 15.5, 3.0, lambda t: t == "Top surface", thin=0.55))
    _add("bottom_layer_direction_" + _v, 0, _plan("direction", _v, 15.5, 0.2, lambda t: t == "Bottom surface", thin=0.55))

def _expansion(value):
    """The plate's top layer, with the outline of the peg and the rib standing on it drawn over it."""
    def picture(k, n):
        layers = _case("top_surface_expansion", value)
        plan = Plan(24.0, SIZE)
        top = _at(layers, 2.0)
        plan.beads(_paths(top, _is_wall), WALL)
        plan.beads(_paths(top, lambda t: not _is_wall(t) and t != "Top surface"), DIM, thin=0.55)
        plan.beads(_paths(top, lambda t: t == "Top surface"), LINE, thin=0.55)
        plan.beads(_paths(_at(layers, 2.2), _is_wall), (235, 242, 242), edge=None, thin=0.45)
        return plan.finish()
    return picture


for _v in ("0", "inward_and_outward", "inward", "outward", "margin"):
    _add("top_surface_expansion_" + _v, 0, _expansion(_v))
for _v in ("each_surface", "each_model", "each_assembly"):
    _add("center_of_surface_pattern_" + _v, 0, _plan("center_of_surface_pattern", _v, 23.0, 3.0, lambda t: t == "Top surface", thin=0.55))
for _v in ("0", "50"):
    _add("top_bottom_infill_wall_overlap_" + _v, 0, _plan("top_bottom_infill_wall_overlap", _v, 5.0, 2.0,
                                                          lambda t: t == "Top surface", centre=(-5.0, -5.0)))

# Sparse infill.
for _v in ("10%", "25%", "50%"):
    _add("sparse_infill_density_" + _slug(_v), 0, _plan("sparse_infill_density", _v, 21.5, 5.0, SPARSE, under=1))
for _v in ("1", "2", "3"):
    _add("fill_multiline_" + _v, 0, _plan("fill_multiline", _v, 11.0, 5.0, SPARSE, centre=(-4.5, -4.5), thin=0.7))
for _v in ("0%", "100%"):
    _add("sparse_infill_smooth_factor_" + _slug(_v), 0, _plan("sparse_infill_smooth_factor", _v, 11.0, 5.0, SPARSE, centre=(-4.5, -4.5)))
for _v in ("none", "template"):
    _add("infill_rotate_template_" + _v, 0, _plan("infill_direction_template", _v, 21.5, 5.0, SPARSE, under=7))
for _v in ("0", "1.5"):
    _add("infill_shift_step_" + _slug(_v), 0, _plan("infill_shift_step", _v, 21.5, 5.0, SPARSE, under=5))
for _v in ("default", "skin_density", "skeleton_density", "skin_depth", "skin_width", "skeleton_width"):
    _add("lockedzag_" + _v, 0, _plan("lockedzag", _v, 21.5, 5.0, SPARSE))
for _v in "disabled distance_warp smooth_blend stepped_shells lobes normal_z normal_y normal_x".split():
    _add("adaptive_infill_mode_" + _v, 0, _plan("adaptive_infill_mode", _v, 25.5, 12.0, SPARSE))
for _v in ("5%", "20%"):
    _add("adaptive_infill_interior_density_" + _slug(_v), 0, _plan("adaptive_infill_interior_density", _v, 25.5, 12.0, SPARSE))
for _v in ("linear", "quadratic", "exponential"):
    _add("adaptive_infill_gradient_" + _v, 0, _plan("adaptive_infill_gradient", _v, 25.5, 12.0, SPARSE))
for _v, _n in (("0", "off"), ("1", "on")):
    _add("gyroid_optimized_" + _n, 0, _section("gyroid_optimized", _v, 13.0, (0.0, 6.0),
                                                lambda z, p: LINE if p["t"] == "Sparse infill" else (WALL if _is_wall(p["t"]) else DIM)))
for _v in ("default", "angle_1", "angle_2"):
    _add("lateral_lattice_" + _v, 0, _section("lateral_lattice", _v, 11.0, (0.0, 5.0),
                                               lambda z, p: LINE if p["t"] == "Sparse infill" else (WALL if _is_wall(p["t"]) else DIM)))
for _v in ("60", "30"):
    _add("infill_overhang_angle_" + _v, 0, _section("infill_overhang_angle", _v, 11.0, (0.0, 5.0),
                                                     lambda z, p: LINE if p["t"] == "Sparse infill" else (WALL if _is_wall(p["t"]) else DIM)))
for _v, _n in (("0", "off"), ("1", "on")):
    _add("infill_complete_top_" + _n, 0, _section("infill_complete_top", _v, 11.0, (0.0, 5.0),
                                                   lambda z, p: LINE if p["t"] == "Sparse infill" else (WALL if _is_wall(p["t"]) else DIM)))
for _v in ("default", "overhang", "prune", "straightening"):
    _add("lightning_" + _v, 0, _lightning(_v))
for _v in ("none", "default"):
    _add("infill_anchor_max_" + _v, 0, _plan("infill_anchor", _v, 8.0, 5.0, SPARSE, centre=(-6.0, -6.0)))
for _v in ("0", "60"):
    _add("infill_wall_overlap_" + _v, 0, _plan("infill_wall_overlap", _v, 6.0, 5.0, SPARSE, centre=(-7.0, -7.0)))
for _v, _n in (("0", "off"), ("1", "on")):
    _add("separated_infills_" + _n, 0, _plan("separated_infills", _v, 24.0, 5.0, SPARSE))

# Solid infill and gaps.
for _v in ("everywhere", "topbottom", "nowhere", "filtered"):
    _add("gap_fill_" + _v, 0, _gap_fill(_v))
for _v, _n in (("0", "off"), ("1", "on")):
    _add("detect_narrow_internal_solid_infill_" + _n, 0, _plan("detect_narrow_internal_solid_infill", _v, 9.0, 3.6,
                                                               lambda t: t == "Internal solid infill", centre=(-4.0, -6.7)))

# Advanced.
for _v, _n in (("0", "off"), ("1", "on")):
    _add("align_infill_direction_to_model_" + _n, 0, _plan("align_infill_direction_to_model", _v, 19.0, None, lambda t: t == "Top surface", thin=0.55))
_add("extra_solid_infills_none", 0, _section("extra_solid_infills", "", 13.5, (0.0, 3.0),
                                             lambda z, p: LINE if p["t"] in SOLID and 0.7 < z < 5.3 else (WALL if _is_wall(p["t"]) else DIM)))
_add("extra_solid_infills_8_2", 0, _section("extra_solid_infills", "8#2", 13.5, (0.0, 3.0),
                                            lambda z, p: LINE if p["t"] in SOLID and 0.7 < z < 5.3 else (WALL if _is_wall(p["t"]) else DIM)))
for _v in ("0", "30", "relative"):
    _add("bridge_angle_" + _v, 0, _plan("bridge_angle", _v, 23.0, 6.2, lambda t: t == "Bridge", thin=0.55))
for _v in ("0", "45"):
    _add("internal_bridge_angle_" + _v, 0, _plan("internal_bridge_angle", _v, 21.5, 7.4, lambda t: t == "Internal Bridge",
                                                 dim=lambda t: not _is_wall(t), under=1, under_test=SPARSE))
for _v in ("0", "15"):
    _add("minimum_sparse_infill_area_" + _v, 0, _plan("minimum_sparse_infill_area", _v, 23.0, 3.0,
                                                      lambda t: t in ("Sparse infill", "Internal solid infill"), centre=(0.0, 0.0)))
for _v in ("off", "100%", "50%"):
    _add("infill_combination_" + _slug(_v), 0, _section("infill_combination", _v, 6.5, (-3.0, 2.5),
                                                        lambda z, p: LINE if p["t"] == "Sparse infill" else (WALL if _is_wall(p["t"]) else DIM)))
for _v in ("none", "ensure_critical_only", "ensure_moderate", "ensure_all"):
    _add("ensure_vertical_shell_" + _v, 0, _section("ensure_vertical_shell_thickness", _v, 17.0, (0.0, 3.8),
                                                    lambda z, p: LINE if p["t"] in SOLID else (WALL if _is_wall(p["t"]) else DIM), axis="x"))
