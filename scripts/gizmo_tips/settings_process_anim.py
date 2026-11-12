"""Animated pictures for the Process settings whose point is an order, a timing or a movement: the order the
parts are printed in, the wipe at the end of a wall, the nozzle speeding up and slowing down, the trip to
the chute for a timelapse snapshot, the order of tool changes.

Each replaces a still drawn by another settings_process* module and keeps its camera, palette and source:
where the still was drawn from G-code that OrcaSlicer sliced, the animation follows that same toolpath, in
the order and (roughly) at the relative speeds the slicer gives it, with the nozzle as a light dot. ACCENT
is what moves or matters, as in the stills. Two values of a setting shown side by side share one time
scale, so the one that takes longer is seen to.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, GIF_SIZE, GRID, PART, PART2, SS
from .settings_process import EDGE, LINE, WALL, Model, Plan, _paths, _rgb
from .settings_process_others import ACC, _fit
from .settings_process_others import _case as _others_case
from .settings_process_quality import MOVE
from .settings_process_quality import _case as _quality_case
from .settings_process_speed import DIM, FILL, GREY, Canvas, _profile, _zigzag
from .settings_process_speed import LINE as GRAPH_LINE
from .settings_process_speed import _fit as _fit_time

OUT = "settings/process"  # resources/images/<OUT>

NOZZLE = (235, 242, 242)
STEPS, HOLD = 18, 4  # frames of motion, then frames that hold the finished state

FRAMES = {}


def _add(name, fn, frames=STEPS + HOLD):
    FRAMES[name] = frames
    globals()[name] = fn


def _clock(k):
    """How far through the motion frame k is, 0..1; the held frames stay at 1."""
    return min(k / float(STEPS - 1), 1.0)


# --- toolpaths against time -----------------------------------------------------------------------


class Timeline:
    """Paths in the order they are printed, sampled densely, each sample with the time the nozzle gets
    there. Travel moves are faster than extrusions. With a window (centre, half size), only the time spent
    inside it counts, and each time the nozzle leaves it a short pause (`gap`) is added, so a loop that goes
    the long way round out of sight takes a moment, not the whole animation."""

    SPEED = {"Travel": 3.0, "Wipe": 1.2}
    STEP = 0.02  # mm between samples

    def __init__(self, paths, window=None, gap=0.0):
        self.paths, self.samples, self.times = paths, [], []
        clock, inside_before = 0.0, False
        for p in paths:
            pts = p["pts"]
            dense, vertex = [pts[:1]], [np.array([True])]
            for a, b in zip(pts[:-1], pts[1:]):
                n = max(1, int(math.ceil(np.linalg.norm(b - a) / self.STEP)))
                dense.append(a + (b - a) * (np.arange(1, n + 1) / n)[:, None])
                vertex.append(np.arange(1, n + 1) == n)
            dense, vertex = np.concatenate(dense), np.concatenate(vertex)
            ds = np.concatenate([[0.0], np.linalg.norm(np.diff(dense, axis=0), axis=1)])
            if window is None:
                inside = np.ones(len(dense), bool)
            else:
                centre, half = window
                inside = np.all(np.abs(dense - np.asarray(centre)) <= half, axis=1)
            t = np.empty(len(dense))
            for i in range(len(dense)):
                if inside[i]:
                    clock += ds[i] / self.SPEED.get(p["t"], 1.0)
                elif inside_before:
                    clock += gap
                inside_before = inside[i]
                t[i] = clock
            self.samples.append((dense, inside, vertex))
            self.times.append(t)
        self.total = clock

    def drawn(self, t):
        """The paths as far as the nozzle has got at time t, and where the nozzle is (None if out of the
        window or not started)."""
        out, nozzle = [], None
        for p, (dense, inside, vertex), times in zip(self.paths, self.samples, self.times):
            n = int(np.searchsorted(times, t, side="right"))
            if n == 0:
                break
            # The path's own corners so far and the nozzle's point: drawn through every sample, a bead's rim
            # would show between them.
            keep = vertex[:n].copy()
            keep[0] = keep[n - 1] = True
            out.append(dict(p, pts=dense[:n][keep]))
            nozzle = dense[n - 1] if inside[n - 1] else None
            if n < len(dense):
                break
        return out, nozzle


# --- wipe: the end of the outer wall, close up ----------------------------------------------------
#
# The stills' window on the seam of a tube's outer wall (settings_process_quality.py). The nozzle closes the
# loop, then wipes (ACCENT) without extruding and travels off; or, with "Wipe before external loop", pushes
# the filament back in away from the wall (the dot) before it moves on to start the wall.

WIPE_SPAN = 3.4


def _wipe(case, value, centre):
    layer = _quality_case(case, value)[1]
    paths = _paths(layer, lambda t: True)
    before_external = case == "wipe_before_external_loop"
    # The animation starts with the travel to the wall it shows: whatever was printed earlier is there already.
    first = next(i for i, p in enumerate(paths) if p["t"] == "Outer wall") - 1
    if before_external:
        first = 1
    last = max(i for i, p in enumerate(paths) if p["t"] == "Wipe") + 1
    earlier = [p for p in paths[:first] if p["t"] not in ("Travel", "Wipe")]
    later = [p for p in paths[last + 1:] if p["t"] not in ("Travel", "Wipe")]
    timeline = Timeline(paths[first:last + 1], (centre, WIPE_SPAN * 0.62), gap=0.5)
    # Where the filament goes back in: at the end of the travel before the wall, or at the wall's start.
    outer = next(i for i, p in enumerate(paths) if p["t"] == "Outer wall")
    prime = paths[outer - 1]["pts"][0] if before_external and value == "1" else paths[outer]["pts"][0]

    def picture(k, n):
        t = _clock(k) * timeline.total
        plan = Plan(WIPE_SPAN, GIF_SIZE, centre=centre)
        plan.beads(earlier, _rgb(PART, 0.75))
        done, nozzle = timeline.drawn(t)
        if k >= STEPS:
            plan.beads(later, _rgb(PART, 0.75))
        plan.beads([p for p in done if p["t"] not in ("Travel", "Wipe", "Outer wall")], _rgb(PART, 0.75))
        plan.beads([p for p in done if p["t"] == "Outer wall"], WALL)
        plan.beads([p for p in done if p["t"] == "Travel"], MOVE, edge=None, thin=0.25)
        plan.beads([p for p in done if p["t"] == "Wipe"], _rgb(ACCENT, 1.4), thin=0.3)
        if before_external and any(p["t"] == "Outer wall" for p in done):
            plan.dot(prime, 3.8, _rgb(ACCENT, 1.4))
        if nozzle is not None and k < STEPS:
            plan.dot(nozzle, 3.4, NOZZLE, outline=EDGE)
        return plan.finish()
    return picture


for _v, _n in (("0", "off"), ("1", "on")):
    _add("wipe_on_loops_" + _n, _wipe("wipe_on_loops", _v, (-0.6, 6.4)))
    _add("wipe_inward_" + _n, _wipe("wipe_inward", _v, (-0.6, 6.4)))
    _add("wipe_before_external_" + _n, _wipe("wipe_before_external_loop", _v, (0.6, 6.2)))


# --- print_order: one layer of nine small parts, visited in order ---------------------------------
#
# The walls of the layer as sliced, in the order printed, and the nozzle's travel from one to the next
# (ACCENT). Both orders run on one clock: the shorter way is done sooner.

ORDER_SPAN = 34.0


def _order_paths(value):
    walls = _paths(_others_case("print_order", value)[0], lambda t: True)
    out = []
    for i, w in enumerate(walls):
        if i:
            out.append(dict(t="Travel", w=0.2, pts=np.array([walls[i - 1]["pts"][-1], w["pts"][0]])))
        out.append(w)
    return walls, out


def _print_order(value):
    walls, paths = _order_paths(value)
    timeline = Timeline(paths)
    total = max(Timeline(_order_paths(v)[1]).total for v in ("default", "as_obj_list"))

    def picture(k, n):
        plan = Plan(ORDER_SPAN, GIF_SIZE)
        plan.beads(walls, _rgb(PART2, 0.75), edge=None)  # the parts still to print
        done, nozzle = timeline.drawn(_clock(k) * total)
        travel = [p for p in done if p["t"] == "Travel"]
        for p in travel:
            plan.draw.line(plan.xy(p["pts"]), fill=LINE, width=int(2.4 * SS * 2))
        plan.beads([p for p in done if p["t"] != "Travel"], WALL)
        plan.dot(walls[0]["pts"][0], 2.4, NOZZLE)  # where the layer starts
        if nozzle is not None and k < STEPS and len(done) < len(paths):
            plan.dot(nozzle, 3.0, NOZZLE, outline=EDGE)
        return plan.finish()
    return picture


# Best of and Snake take the same way as Default on a layout this small, so only these two are drawn.
for _v in ("default", "as_obj_list"):
    _add("print_order_" + _v, _print_order(_v), STEPS + 6)


# --- print_sequence: two parts growing, together or one after the other --------------------------
#
# The two tubes of the still (settings_process_others.py): by layer, each layer of both before the next; by
# object, the first part to the top before the second starts. The layer being printed is in ACCENT.

SEQ_STEPS = 18


def _print_sequence(by_object):
    layers = _others_case("print_sequence", "parts")
    view = _fit([(layer["z"], 0.2, p, PART) for layer in layers for p in _paths(layer, lambda t: True)])
    count = len(layers)

    def picture(k, n):
        f = min((k + 1) / float(SEQ_STEPS), 1.0)
        if by_object:
            tops = (min(2.0 * f, 1.0), max(2.0 * f - 1.0, 0.0))  # the first part, then the second
        else:
            tops = (f, f)
        printing = k < SEQ_STEPS
        live = 1 if by_object and tops[0] >= 1.0 else 0  # the part the nozzle is on
        items, nozzle = [], None
        for i, layer in enumerate(layers):
            for p in _paths(layer, lambda t: True):
                part = 0 if p["pts"][:, 0].mean() < 0.0 else 1
                top = int(round(tops[part] * count))
                if i >= top:
                    continue
                current = printing and i == top - 1 and (part == live or not by_object)
                items.append((layer["z"], 0.2, p, ACC if current else PART))
                if current and part == live and p["t"] == "Outer wall":
                    pts = p["pts"]
                    nozzle = (pts[(k * 7) % len(pts)], layer["z"])
        model = Model(items, GIF_SIZE, **view)
        if nozzle is not None:
            (x, y), z = nozzle
            model.dot(x, y, z + 0.4, 3.0, NOZZLE, outline=_rgb(BED, 0.5))
        return model.finish()
    return picture


_add("print_sequence_by_layer", _print_sequence(False), SEQ_STEPS + HOLD)
_add("print_sequence_by_object", _print_sequence(True), SEQ_STEPS + HOLD)


# --- acceleration, jerk, accel to decel: the nozzle along its moves, over its speed ----------------
#
# The stills' speed graphs (settings_process_speed.py), drawn as time goes on: the nozzle (light dot) moves
# along the path at the top at the speed the graph shows under it. The other value's curve stays dashed, so
# the one that gets there first is seen to.


def _speed_at(curve, t):
    xs, ys = zip(*curve)
    return float(np.interp(t, xs, ys))


def _distance(curve, t):
    """How far the nozzle has gone by time t: the area under the speed curve."""
    xs = np.linspace(0.0, t, 200)
    return float(np.trapezoid([_speed_at(curve, x) for x in xs], xs)) if t > 0 else 0.0


def _upto(curve, t):
    pts = [(x, y) for x, y in curve if x < t]
    return pts + [(t, _speed_at(curve, t))]


def _speed_anim(curve, ghost, path, times, corners):
    """path: the moves' corners; times: when the nozzle is at each."""
    span = max(curve[-1][0], ghost[-1][0])
    ends = [_distance(curve, x) for x in times]

    def picture(k, n):
        t = _clock(k) * span
        c = Canvas(GIF_SIZE, box=(0.12, 0.12, 0.88, 0.62))
        c.axes()
        c.dashed(ghost, DIM, 1.3)
        now = min(t, curve[-1][0])
        drawn = _upto(curve, now)
        if now > 0.0:
            c.area(drawn, FILL)
            c.line(drawn, GRAPH_LINE, 2.4)
        top = Canvas(GIF_SIZE, box=(0.12, 0.70, 0.88, 0.92))
        top.image, top.draw = c.image, c.draw
        top.line(path, GREY, 2.2)
        for x, y in corners:
            top.dot(x, y, 3.2, GRAPH_LINE)
        # The nozzle: which move it is on, and how far along it.
        d = _distance(curve, now)
        i = max(j for j in range(len(ends)) if ends[j] <= d + 1e-9)
        if i >= len(path) - 1:
            pos = path[-1]
        else:
            f = (d - ends[i]) / max(ends[i + 1] - ends[i], 1e-9)
            pos = tuple(np.asarray(path[i]) + (np.asarray(path[i + 1]) - np.asarray(path[i])) * f)
        top.dot(pos[0], pos[1], 3.6, NOZZLE, outline=_rgb(BED, 0.5))
        c.dot(now, _speed_at(curve, now), 2.6, NOZZLE)
        return c.finish()
    return picture


def _accel(a):
    moves = [(1.0, 0.85, 0.0)]
    other = 9.0 if a < 3.0 else 1.6
    curve, ghost = _profile(moves, a, 1.6), _profile(moves, other, 1.6)
    return _speed_anim(curve, ghost, [(0.0, 0.5), (1.0, 0.5)], [0.0, curve[-1][0]], [(0.0, 0.5), (1.0, 0.5)])


def _zig_anim(curve, ghost, corners_from):
    zig = _zigzag(curve)
    return _speed_anim(curve, ghost, zig, [x for x, _ in zig], zig[corners_from:len(zig) - corners_from])


def _corners(vj):
    other = 0.6 if vj < 0.3 else 0.08
    curve, ghost = _fit_time(*(_profile([(0.5, 0.85, v)] * 3 + [(0.5, 0.85, 0.0)], 4.0, 1.0) for v in (vj, other)))
    return _zig_anim(curve, ghost, 1)


def _cruise(peak):
    other = 0.85 if peak < 0.6 else 0.45
    curve, ghost = _fit_time(*(_profile([(0.22, p, 0.0)] * 5, 5.0, 1.0) for p in (peak, other)))
    return _zig_anim(curve, ghost, 0)


_add("speed_accel_low", _accel(1.6))
_add("speed_accel_high", _accel(9.0))
_add("speed_jerk_low", _corners(0.08))
_add("speed_jerk_high", _corners(0.6))
_add("speed_accel_to_decel_low", _cruise(0.45))
_add("speed_accel_to_decel_high", _cruise(0.85))


# --- timelapse: what the toolhead does for each snapshot ------------------------------------------
#
# The bed from above (settings_process_extra.py's still). The nozzle prints a layer round the part, then the
# snapshot (the bed flashes): traditional takes it with the nozzle where it is; smooth sends the toolhead to
# the chute (ACCENT, the way there and back) and wipes it on the prime tower on its way back.


def _timelapse(smooth_mode):
    def picture(k, n):
        px = GIF_SIZE * SS * 2
        img = Image.new("RGB", (px, px), _rgb(BED, 0.75))
        draw = ImageDraw.Draw(img)
        centre, r_part = (0.48 * px, 0.52 * px), 0.12 * px
        tower = (0.73 * px, 0.37 * px)
        chute = (0.95 * px, 0.06 * px)
        orbit = 10
        if smooth_mode:
            # print, to the chute, snapshot, back to the tower, wipe, back to the part
            legs = [("print", orbit), ("out", 3), ("snap", 3), ("back", 2), ("wipe", 2), ("in", 2)]
        else:
            legs = [("print", orbit), ("snap", 4)]
        at, leg, f = 0, None, 0.0
        for name, length in legs:
            if k < at + length:
                leg, f = name, (k - at) / float(length)
                break
            at += length
        flash = leg == "snap" and f < 0.5
        draw.rectangle([0.1 * px, 0.1 * px, 0.9 * px, 0.9 * px], fill=_rgb(GRID, 2.6 if flash else 1.1))
        draw.ellipse([centre[0] - r_part, centre[1] - r_part, centre[0] + r_part, centre[1] + r_part], fill=_rgb(PART),
                     outline=_rgb(BED, 0.5), width=SS * 2)
        rim = lambda a: (centre[0] + 0.7 * r_part * math.cos(a), centre[1] + 0.7 * r_part * math.sin(a))  # noqa: E731
        home = rim(-0.6)  # where the layer ends and the next begins
        if smooth_mode:
            draw.rectangle([tower[0] - 0.07 * px, tower[1] - 0.07 * px, tower[0] + 0.07 * px, tower[1] + 0.07 * px],
                           fill=_rgb(ACCENT, 1.25 if leg == "wipe" else 0.8), outline=_rgb(BED, 0.5), width=SS * 2)
            way = [home, chute, tower, home]
            for a, b in zip(way[:-1], way[1:]):  # the way there and back, dashed
                for s in np.arange(0.0, 1.0, 0.1):
                    e = min(s + 0.055, 1.0)
                    draw.line([(a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s),
                               (a[0] + (b[0] - a[0]) * e, a[1] + (b[1] - a[1]) * e)], fill=_rgb(ACCENT, 1.25),
                              width=int(1.6 * SS * 2))
        if leg == "print":
            a = -0.6 + 2.0 * math.pi * f
            nozzle = rim(a)
            arc = [rim(-0.6 + 2.0 * math.pi * f * i / 24.0) for i in range(25)]
            draw.line(arc, fill=_rgb(PART, 1.3), width=int(2.4 * SS * 2), joint="curve")
        elif leg == "snap":
            nozzle = chute if smooth_mode else home
        elif leg == "out":
            nozzle = tuple(np.asarray(home) + (np.asarray(chute) - np.asarray(home)) * f)
        elif leg == "back":
            nozzle = tuple(np.asarray(chute) + (np.asarray(tower) - np.asarray(chute)) * f)
        elif leg == "wipe":
            nozzle = (tower[0] + 0.05 * px * math.sin(2.0 * math.pi * f), tower[1])
        else:
            nozzle = tuple(np.asarray(tower) + (np.asarray(home) - np.asarray(tower)) * f)
        r = 0.035 * px
        draw.ellipse([nozzle[0] - r, nozzle[1] - r, nozzle[0] + r, nozzle[1] + r], fill=NOZZLE, outline=_rgb(BED, 0.5),
                     width=SS * 2)
        return np.asarray(img.resize((GIF_SIZE, GIF_SIZE), Image.LANCZOS))
    return picture


_add("timelapse_traditional", _timelapse(False), 14)
_add("timelapse_smooth", _timelapse(True), 22)


# --- toolchange_ordering: the filaments of each layer, in the order they are printed --------------
#
# The still's grid (settings_process_others.py): a row per layer from the bottom up, a block per filament in
# the order printed, joined by the nozzle's way. The blocks come in one after the other, so the order reads
# without arrows: Default starts each layer with the filament the last one ended on.


TOOLS = [_rgb(ACCENT, 1.25), _rgb(PART), _rgb(PART2, 1.1)]  # the three filaments


def _toolchanges(rows):
    order = [(r, i) for r in range(len(rows)) for i in range(len(rows[r]))]
    steps = len(order)

    def picture(k, n):
        s = GIF_SIZE * SS
        img = Image.new("RGB", (s, s), _rgb(BED, 0.75))
        draw = ImageDraw.Draw(img)
        cell, gap = 0.17 * s, 0.11 * s
        x0 = 0.5 * s - 1.5 * cell - gap
        shown = min(k + 1, steps)
        # The still's grid, a little closer together, so the top row clears the picture's rounded edge.
        centre = lambda r, i: (x0 + i * (cell + gap) + cell / 2, s * 0.915 - r * (cell + 0.05 * s) - cell / 2)  # noqa: E731
        way = []
        for j, (r, i) in enumerate(order[:shown]):
            if j and r != order[j - 1][0]:  # the step up to the next layer
                (px_, py_), (_, ny) = centre(*order[j - 1]), centre(r, i)
                way += [(px_ + cell * 0.75, py_), (px_ + cell * 0.75, ny)]
            way.append(centre(r, i))
        for r, row in enumerate(rows):
            for i in range(len(row)):
                cx, cy = centre(r, i)
                draw.rounded_rectangle([cx - cell / 2, cy - cell / 2, cx + cell / 2, cy + cell / 2], radius=0.12 * cell,
                                       outline=_rgb(PART2, 0.8), width=SS)
        if len(way) > 1:
            draw.line(way, fill=_rgb(PART, 0.55), width=int(2.2 * SS), joint="curve")
        for j, (r, i) in enumerate(order[:shown]):
            cx, cy = centre(r, i)
            draw.rounded_rectangle([cx - cell / 2, cy - cell / 2, cx + cell / 2, cy + cell / 2], radius=0.12 * cell,
                                   fill=TOOLS[rows[r][i]], outline=_rgb(BED, 0.5), width=SS)
            if j == shown - 1 and k < steps:  # the block being printed
                draw.rounded_rectangle([cx - cell / 2 - SS * 2, cy - cell / 2 - SS * 2, cx + cell / 2 + SS * 2,
                                        cy + cell / 2 + SS * 2], radius=0.16 * cell, outline=NOZZLE, width=SS * 2)
        return np.asarray(img.resize((GIF_SIZE, GIF_SIZE), Image.LANCZOS))
    return picture


_add("toolchange_default", _toolchanges([[0, 1, 2], [2, 0, 1], [1, 2, 0], [0, 1, 2]]), 12 + 6)
_add("toolchange_cyclic", _toolchanges([[0, 1, 2]] * 4), 12 + 6)
_add("toolchange_first_layer_off", _toolchanges([[1, 2, 0], [0, 1, 2], [0, 1, 2], [0, 1, 2]]), 12 + 6)
