"""Pictures for the tooltips of the printer settings (SettingTipsPrinter.cpp).

Each one follows the code that reads the setting, so what it shows is what the slicer does:

- The Z-hop types are GCodeWriter::travel_to_xyz(): Normal lifts in place, Slope rises along the travel
  at the traveling angle until it reaches the hop height, Spiral rises along one full circle beside the
  start point and then travels level. Auto (GCode::retract and the travel planner) picks Slope for a
  travel, and Spiral at layer changes and where the travel starts over an overhang. Each travel ends by
  coming straight down.
- The Z-hop limits and surfaces are GCode::retract(): a lift happens between the lower and the upper
  boundary only, and "On surfaces" keeps it to the first layer, to travels leaving a top surface, or both.
- The clearances are Print::sequential_print_horizontal_clearance_valid() and the vertical check after it:
  in by-object printing an object already printed has to stay out of the radius around the nozzle, and
  below the rod (or the lid) while the toolhead prints the next one. Objects lower than nozzle_height
  meet only the nozzle cone, so they can stand much closer.
- The wipe is Wipe::wipe(): after a loop the nozzle carries on along the start of the loop it just closed.
- The adaptive bed mesh probes the objects' bounding box grown by the margin.

Paths, areas and distances are drawn over a ray-marched scene: the grey parts and the dark bed are the
plater's, ACCENT marks what the setting changes.
"""

import math

import numpy as np

from .render import (ACCENT, AXIS_X, AXIS_Y, GIF_SIZE, M_ACCENT, M_PART, M_PART2, SIZE, Camera, Overlay, bed, phase,
                     render, rot_x, rot_y, rot_z, sd_box, sd_cylinder, smooth, union)

OUT = "settings/printer"  # resources/images/<OUT>

TEAL = tuple(int(round(c * 255)) for c in ACCENT)
TEAL_LIGHT = (70, 205, 185)
TEAL_FILL = TEAL + (70,)
PATH = (214, 220, 220)  # a travel or an extrusion the setting leaves alone
PATH_DIM = (150, 156, 158)
TRAVEL_ON_PART = (62, 66, 68)  # a travel drawn over a lit grey top
OUTLINE = (225, 230, 230)


# --- shapes ---------------------------------------------------------------------------------------


def sd_capped_cone(p, tip, r0, r1, h):
    """An upright cone frustum from radius r0 at z = tip.z to r1 at tip.z + h, centred on tip in x and y."""
    q = p - np.asarray(tip, float)
    rq = np.hypot(q[:, 0], q[:, 1])
    z = q[:, 2] - 0.5 * h
    hh = 0.5 * h
    # iq's capped cone, in the (radius, height) half plane.
    k1 = np.array([r1, hh])
    k2 = np.array([r1 - r0, 2.0 * hh])
    ca_r = rq - np.minimum(rq, np.where(z < 0.0, r0, r1))
    ca = np.stack([ca_r, np.abs(z) - hh], axis=1)
    qq = np.stack([rq, z], axis=1)
    t = np.clip(((k1 - qq) @ k2) / (k2 @ k2), 0.0, 1.0)
    cb = qq - k1 + t[:, None] * k2
    s = np.where((cb[:, 0] < 0.0) & (ca[:, 1] < 0.0), -1.0, 1.0)
    return s * np.sqrt(np.minimum((ca * ca).sum(axis=1), (cb * cb).sum(axis=1)))


def sd_rod_x(p, y, z, radius, x0, x1):
    """A round rod along X."""
    dx = np.maximum(np.maximum(x0 - p[:, 0], p[:, 0] - x1), 0.0)
    return np.sqrt(dx * dx + (p[:, 1] - y) ** 2 + (p[:, 2] - z) ** 2) - radius


def hotend(p, tip, scale=1.0):
    """Nozzle cone, heater block and heat sink, the nozzle's tip at `tip`."""
    tip = np.asarray(tip, float)
    s = scale
    cone = sd_capped_cone(p, tip, 0.025 * s, 0.10 * s, 0.13 * s)
    block = sd_box(p, tip + (0.0, 0.0, 0.13 * s + 0.08 * s), (0.17 * s, 0.13 * s, 0.08 * s), 0.015 * s)
    sink = sd_cylinder(p, tip[:2], 0.08 * s, tip[2] + 0.29 * s, tip[2] + 0.52 * s, 0.01 * s)
    return np.minimum(np.minimum(cone, block), sink)


def toolhead(p, tip):
    """A hotend under a carriage box, for the by-object pictures."""
    tip = np.asarray(tip, float)
    body = sd_box(p, tip + (0.0, 0.0, 0.62), (0.32, 0.26, 0.22), 0.03)
    return np.minimum(hotend(p, tip), body)


def plate(p, half=(1.25, 1.1), z=0.06):
    """A raised build plate on the bed, its top at z."""
    return sd_box(p, (0.0, 0.0, 0.5 * z), (half[0], half[1], 0.5 * z), 0.01)


# --- overlay helpers ------------------------------------------------------------------------------


def dashed(o, a, b, colour, width=1.4, dash=0.06, gap=0.045):
    a, b = np.asarray(a, float), np.asarray(b, float)
    length = float(np.linalg.norm(b - a))
    if length < 1e-9:
        return
    d = (b - a) / length
    t = 0.0
    while t < length:
        o.line(a + d * t, a + d * min(t + dash, length), colour, width)
        t += dash + gap


def dimension(o, a, b, colour=TEAL_LIGHT, width=2.0, tick=0.06, normal=(1.0, 0.0, 0.0)):
    """A measured distance: the line and a tick across each end."""
    a, b, n = np.asarray(a, float), np.asarray(b, float), np.asarray(normal, float)
    o.line(a, b, colour, width)
    for e in (a, b):
        o.line(e - n * tick, e + n * tick, colour, width)


def fill_polygon(o, pts, fill, outline=None, width=1.6):
    xy = [o.pt(q) for q in pts]
    o.draw.polygon(xy, fill=fill)
    if outline is not None:
        o.polyline(pts, outline, width, closed=True)


def path_points(segments, samples=24):
    """Points along a chain of segments [(a, b), ...] or point lists, densely sampled."""
    out = []
    for seg in segments:
        seg = [np.asarray(q, float) for q in seg]
        for a, b in zip(seg[:-1], seg[1:]):
            for t in np.linspace(0.0, 1.0, samples, endpoint=False):
                out.append(a + (b - a) * t)
    out.append(np.asarray(segments[-1][-1], float))
    return out


def cut_path(pts, frac):
    """The part of a polyline up to `frac` of its length, and the point there."""
    pts = [np.asarray(q, float) for q in pts]
    lens = [float(np.linalg.norm(b - a)) for a, b in zip(pts[:-1], pts[1:])]
    total = sum(lens)
    goal = total * min(max(frac, 0.0), 1.0)
    out = [pts[0]]
    for (a, b), l in zip(zip(pts[:-1], pts[1:]), lens):
        if goal <= l:
            end = a + (b - a) * (goal / l if l > 0 else 0.0)
            out.append(end)
            return out, end
        out.append(b)
        goal -= l
    return out, pts[-1]


# --- Z-hop types ------------------------------------------------------------------------------------

HOP_TOP = 0.55  # top of the two blocks
HOP = 0.42  # the lift, exaggerated to read at this size
HOP_A = np.array([-0.78, 0.0, HOP_TOP])
HOP_B = np.array([0.78, 0.0, HOP_TOP])
HOP_SLOPE = math.radians(32.0)


def _hop_blocks(p):
    a = sd_box(p, (-0.85, 0.0, 0.5 * HOP_TOP), (0.33, 0.33, 0.5 * HOP_TOP), 0.02)
    b = sd_box(p, (0.85, 0.0, 0.5 * HOP_TOP), (0.33, 0.33, 0.5 * HOP_TOP), 0.02)
    return np.minimum(a, b)


def _hop_path(kind, a, b):
    up = np.array([0.0, 0.0, HOP])
    d = (b - a) / np.linalg.norm(b - a)
    if kind == "normal":
        return path_points([[a, a + up, b + up, b]])
    if kind == "slope":
        run = HOP / math.tan(HOP_SLOPE)
        return path_points([[a, a + d * run + up, b + up, b]])
    # spiral: one turn around a centre beside the start, rising the whole lift, then level
    r = 0.17
    perp = np.array([-d[1], d[0], 0.0])
    centre = a + perp * r
    start_ang = math.atan2(a[1] - centre[1], a[0] - centre[0])
    turn = []
    for t in np.linspace(0.0, 1.0, 48):
        ang = start_ang - 2.0 * math.pi * t
        turn.append(centre + r * np.array([math.cos(ang), math.sin(ang), 0.0]) + up * t)
    return path_points([turn, [a + up, b + up, b]], samples=6)


def _hop_frame(paths, frac_of_total, size):
    """All `paths` drawn up to a point; the nozzle at that point."""
    lengths = [sum(float(np.linalg.norm(q1 - q0)) for q0, q1 in zip(pp[:-1], pp[1:])) for pp in paths]
    goal = frac_of_total * sum(lengths)
    done, tip = [], paths[0][0]
    for pp, l in zip(paths, lengths):
        if goal >= l:
            done.append(pp)
            tip = pp[-1]
            goal -= l
        else:
            part, tip = cut_path(pp, goal / l)
            done.append(part)
            break
    scene = lambda p: union(bed(p), (_hop_blocks(p), M_PART), (hotend(p, tip, 0.9), M_PART2))
    cam = Camera(size, target=(0.0, 0.0, 0.62), extent=2.75, az=28.0, el=16.0)
    o = Overlay(render(scene, cam), cam)
    for part in done:
        if len(part) > 1:
            o.polyline(part, TEAL_LIGHT, width=2.6)
    return o.finish(size)


def _hop_anim(k, n, paths):
    move = n - 5  # then hold the finished path
    return _hop_frame(paths, smooth(phase(k, 1, move)), GIF_SIZE)


def z_hop_normal(k, n):
    return _hop_anim(k, n, [_hop_path("normal", HOP_A, HOP_B)])


def z_hop_slope(k, n):
    return _hop_anim(k, n, [_hop_path("slope", HOP_A, HOP_B)])


def z_hop_spiral(k, n):
    return _hop_anim(k, n, [_hop_path("spiral", HOP_A, HOP_B)])


def z_hop_auto(k, n):
    # A slope for the travel across, then the spiral Auto forces when the layer changes (GCode::change_layer).
    a = HOP_B
    d = np.array([1.0, 0.0, 0.0])
    r = 0.17
    centre = a + np.array([-d[1], d[0], 0.0]) * r
    start = math.atan2(a[1] - centre[1], a[0] - centre[0])
    turn = [centre + r * np.array([math.cos(start - 2.0 * math.pi * t), math.sin(start - 2.0 * math.pi * t), 0.0]) +
            np.array([0.0, 0.0, HOP * t]) for t in np.linspace(0.0, 1.0, 48)]
    return _hop_anim(k, n, [_hop_path("slope", HOP_A, HOP_B), path_points([turn], samples=2)])


# --- Z-hop limits and surfaces -------------------------------------------------------------------


def _travel(o, z, hop, x0=-0.42, x1=0.42, y=0.0, h=0.16):
    a, b = np.array([x0, y, z]), np.array([x1, y, z])
    if hop:
        up = np.array([0.0, 0.0, h])
        o.polyline([a, a + up, b + up, b], TEAL_LIGHT, width=2.4)
    else:
        o.line(a, b, PATH_DIM, width=1.8)


def _towers(heights, size):
    ha, hb = heights

    def scene(p):
        a = sd_box(p, (-0.72, 0.0, 0.5 * ha), (0.3, 0.3, 0.5 * ha), 0.02)
        b = sd_box(p, (0.72, 0.0, 0.5 * hb), (0.3, 0.3, 0.5 * hb), 0.02)
        return union(bed(p), (np.minimum(a, b), M_PART))

    cam = Camera(size, target=(0.0, 0.0, 0.5 * max(ha, hb) + 0.05), extent=0.95 * max(ha, hb) + 1.35, az=8.0, el=14.0)
    return Overlay(render(scene, cam), cam)


def z_hop_range(k, n):
    """Only lift Z above / below: lifts inside the band between the two heights, flat travels elsewhere."""
    o = _towers((1.6, 1.6), SIZE)
    lo, hi = 0.62, 1.22
    for z0 in (lo, hi):
        pts = [np.array(q) for q in ((-0.42, -0.45, z0), (0.42, -0.45, z0), (0.42, 0.45, z0), (-0.42, 0.45, z0))]
        fill_polygon(o, pts, TEAL + (55,), TEAL, width=1.4)
    for z in (0.12, 0.4, 0.75, 1.0, 1.4):
        _travel(o, z, lo <= z <= hi, h=0.13)
    return o.finish(SIZE)


def _enforce(hop_first, hop_top):
    """Travels between a tall and a short tower: the first layer, a middle layer and the short one's top."""
    o = _towers((1.25, 0.72), SIZE)
    _travel(o, 0.02, hop_first)
    _travel(o, 0.36, False)
    _travel(o, 0.72, hop_top)  # leaving the short tower's top surface
    return o


def retract_lift_all(k, n):
    o = _towers((1.25, 0.72), SIZE)
    for z in (0.02, 0.36, 0.72):
        _travel(o, z, True)
    return o.finish(SIZE)


def retract_lift_top(k, n):
    return _enforce(False, True).finish(SIZE)


def retract_lift_bottom(k, n):
    return _enforce(True, False).finish(SIZE)


def retract_lift_top_bottom(k, n):
    return _enforce(True, True).finish(SIZE)


def travel_slope(k, n):
    """The traveling angle: the slope a lift rises at, measured from the travel's own direction."""
    a, b = HOP_A, HOP_B
    d = (b - a) / np.linalg.norm(b - a)
    run = HOP / math.tan(HOP_SLOPE)
    top = a + d * run + np.array([0.0, 0.0, HOP])
    scene = lambda p: union(bed(p), (_hop_blocks(p), M_PART), (hotend(p, b + (0, 0, HOP), 0.9), M_PART2))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.75, az=12.0, el=17.0)
    o = Overlay(render(scene, cam), cam)
    dashed(o, a, a + d * (run + 0.25), PATH, width=1.3)
    arc = [a + 0.42 * (math.cos(t) * d + math.sin(t) * np.array([0.0, 0.0, 1.0])) for t in np.linspace(0.0, HOP_SLOPE, 24)]
    o.polyline(arc, TEAL, width=2.4)
    o.polyline([a, top, b + (0, 0, HOP)], TEAL_LIGHT, width=2.6)
    return o.finish(SIZE)


# --- the bed ---------------------------------------------------------------------------------------

PLATE_Z = 0.06
PLATE_HALF = (1.25, 1.1)


def _plate_overlay(size, el=58.0, objects=None, target=(0.0, -0.05, 0.0), extent=3.1):
    objects = objects or []

    def scene(p):
        items = [bed(p), (plate(p, PLATE_HALF, PLATE_Z), M_PART2)]
        items += [(f(p), M_PART) for f in objects]
        return union(*items)

    cam = Camera(size, target=target, extent=extent, az=25.0, el=el)
    return Overlay(render(scene, cam), cam)


def _axes(o, origin, length=0.55):
    origin = np.asarray(origin, float)
    o.arrow(origin, origin + (length, 0.0, 0.0), AXIS_X, width=2.4, head=8.0)
    o.arrow(origin, origin + (0.0, length, 0.0), AXIS_Y, width=2.4, head=8.0)
    o.dot(origin, 3.0, OUTLINE)


def printable_area(k, n):
    """The printable area on the plate, with the bed origin at its front left corner."""
    o = _plate_overlay(SIZE)
    x0, y0, x1, y1 = -1.1, -0.95, 1.1, 0.95
    z = PLATE_Z + 0.002
    corners = [np.array(q) for q in ((x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z))]
    fill_polygon(o, corners, TEAL + (75,), TEAL_LIGHT, width=2.0)
    _axes(o, corners[0])
    return o.finish(SIZE)


def bed_origin(k, n):
    """The origin moved into the plate: its distances from the front left corner along X and Y."""
    o = _plate_overlay(SIZE)
    x0, y0, x1, y1 = -1.1, -0.95, 1.1, 0.95
    z = PLATE_Z + 0.002
    corners = [np.array(q) for q in ((x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z))]
    o.polyline(corners, OUTLINE, width=1.4, closed=True)
    origin = np.array([0.0, 0.0, z])
    dimension(o, corners[0] + (0.0, -0.14, 0.0), np.array([0.0, y0 - 0.14, z]), normal=(0.0, 1.0, 0.0))
    dimension(o, corners[0] + (-0.14, 0.0, 0.0), np.array([x0 - 0.14, 0.0, z]), normal=(1.0, 0.0, 0.0))
    dashed(o, np.array([0.0, y0, z]), origin, PATH, width=1.2)
    dashed(o, np.array([x0, 0.0, z]), origin, PATH, width=1.2)
    o.dot(corners[0], 3.2, TEAL_LIGHT)
    _axes(o, origin)
    return o.finish(SIZE)


def bed_exclude_area(k, n):
    """An excluded corner of the plate: the objects are arranged around it."""
    objs = [lambda p: sd_box(p, (0.35, 0.2, PLATE_Z + 0.25), (0.28, 0.28, 0.25), 0.02),
            lambda p: sd_cylinder(p, (-0.45, 0.45), 0.25, PLATE_Z, PLATE_Z + 0.42, 0.02),
            lambda p: sd_box(p, (0.75, -0.5, PLATE_Z + 0.15), (0.22, 0.2, 0.15), 0.02)]
    o = _plate_overlay(SIZE, el=48.0, objects=objs)
    z = PLATE_Z + 0.002
    x0, y0, x1, y1 = -1.25, -1.1, -0.45, -0.35
    area = [np.array(q) for q in ((x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z))]
    fill_polygon(o, area, TEAL + (95,), TEAL_LIGHT, width=2.0)
    # hatching, so the area reads as one that is kept clear
    for t in np.linspace(-0.6, 0.9, 9):
        a = np.array([x0 + t, y0, z])
        b = np.array([x0 + t + (y1 - y0), y1, z])
        # clip the 45 degree line to the rectangle
        s0 = max(0.0, (x0 - a[0]) / (b[0] - a[0]))
        s1 = min(1.0, (x1 - a[0]) / (b[0] - a[0]))
        if s1 > s0:
            o.line(a + (b - a) * s0, a + (b - a) * s1, TEAL_LIGHT + (150,), width=1.2)
    return o.finish(SIZE)


def adaptive_bed_mesh_margin(k, n):
    """Probe points over the objects' bounding box, grown by the margin."""
    objs = [lambda p: sd_cylinder(p, (-0.15, 0.1), 0.32, PLATE_Z, PLATE_Z + 0.3, 0.02),
            lambda p: sd_box(p, (0.42, -0.2, PLATE_Z + 0.18), (0.18, 0.18, 0.18), 0.02)]
    o = _plate_overlay(SIZE, el=68.0, objects=objs, extent=2.9)
    z = PLATE_Z + 0.002
    bx0, by0, bx1, by1 = -0.47, -0.38, 0.6, 0.42  # the objects' bounding box
    m = 0.28
    box = [np.array(q) for q in ((bx0, by0, z), (bx1, by0, z), (bx1, by1, z), (bx0, by1, z))]
    mesh = [np.array(q) for q in ((bx0 - m, by0 - m, z), (bx1 + m, by0 - m, z), (bx1 + m, by1 + m, z), (bx0 - m, by1 + m, z))]
    # the margin: the band between the two rectangles
    band = mesh + [mesh[0], box[0], box[3], box[2], box[1], box[0]]
    o.draw.polygon([o.pt(q) for q in band], fill=TEAL + (85,))
    o.polyline(mesh, TEAL_LIGHT, width=2.0, closed=True)
    for e in range(4):
        dashed(o, box[e], box[(e + 1) % 4], OUTLINE, width=1.1, dash=0.05, gap=0.04)
    for x in np.linspace(bx0 - m, bx1 + m, 5):
        for y in np.linspace(by0 - m, by1 + m, 4):
            o.dot((x, y, z), 2.4, OUTLINE, outline=(40, 44, 46))
    return o.finish(SIZE)


# --- by-object clearances ------------------------------------------------------------------------

FIRST_H = 0.8  # the object printed first, finished
SECOND_H = 0.32  # the one being printed
SECOND_AT = np.array([0.55, 0.0])
FIRST_AT = np.array([-0.85, 0.0])


def _by_object(size, extra=None, cam_kw=None):
    tip = np.array([SECOND_AT[0], SECOND_AT[1], SECOND_H])

    def scene(p):
        first = sd_box(p, (FIRST_AT[0], FIRST_AT[1], 0.5 * FIRST_H), (0.3, 0.3, 0.5 * FIRST_H), 0.02)
        second = sd_cylinder(p, SECOND_AT, 0.28, 0.0, SECOND_H - 0.005, 0.02)
        items = [bed(p), (np.minimum(first, second), M_PART), (toolhead(p, tip), M_PART2)]
        if extra is not None:
            items.append((extra(p), M_PART2))
        return union(*items)

    kw = dict(target=(-0.1, 0.0, 0.7), extent=3.0, az=22.0, el=24.0)
    kw.update(cam_kw or {})
    cam = Camera(size, **kw)
    return Overlay(render(scene, cam), cam), tip


def extruder_clearance_radius(k, n):
    o, tip = _by_object(SIZE, cam_kw=dict(el=34.0, target=(0.05, 0.0, 0.55), extent=3.5))
    r = 1.05
    centre = np.array([tip[0], tip[1], SECOND_H + 0.01])
    ring = [centre + r * np.array([math.cos(a), math.sin(a), 0.0]) for a in np.linspace(0.0, 2.0 * math.pi, 72)]
    o.draw.polygon([o.pt(q) for q in ring], fill=TEAL + (60,))
    o.polyline(ring, TEAL_LIGHT, width=2.0, closed=True)
    dimension(o, centre, centre + (0.0, -r, 0.0), normal=(1.0, 0.0, 0.0))
    return o.finish(SIZE)


ROD_ABOVE = 0.95  # rod height above the nozzle tip
LID_ABOVE = 1.35


def extruder_clearance_height_to_rod(k, n):
    rod_z = SECOND_H + ROD_ABOVE
    rod = lambda p: sd_rod_x(p, 0.38, rod_z + 0.07, 0.07, -1.9, 1.9)
    o, tip = _by_object(SIZE, extra=rod, cam_kw=dict(el=18.0, az=18.0, target=(-0.1, 0.0, 0.8)))
    dimension(o, tip + (0.42, 0.38, 0.0), tip + (0.42, 0.38, ROD_ABOVE))
    dashed(o, tip, tip + (0.42, 0.38, 0.0), PATH, width=1.2)
    return o.finish(SIZE)


def extruder_clearance_height_to_lid(k, n):
    o, tip = _by_object(SIZE, cam_kw=dict(el=18.0, az=18.0, target=(-0.1, 0.0, 0.85), extent=3.2))
    lid_z = SECOND_H + LID_ABOVE
    lid = [np.array(q) for q in ((-1.6, -0.9, lid_z), (1.6, -0.9, lid_z), (1.6, 0.9, lid_z), (-1.6, 0.9, lid_z))]
    fill_polygon(o, lid, (200, 206, 206, 45), OUTLINE, width=1.4)
    dimension(o, tip + (0.45, 0.0, 0.0), tip + (0.45, 0.0, LID_ABOVE))
    dashed(o, tip, tip + (0.45, 0.0, 0.0), PATH, width=1.2)
    return o.finish(SIZE)


def nozzle_height(k, n):
    """Objects lower than the nozzle's reach below the hotend stand right beside it."""
    tip = np.array([0.0, 0.0, 0.2])
    low = 0.17

    def scene(p):
        a = sd_box(p, (-0.32, 0.05, 0.5 * low), (0.15, 0.15, 0.5 * low), 0.015)
        b = sd_box(p, (0.36, -0.02, 0.5 * low), (0.15, 0.15, 0.5 * low), 0.015)
        printing = sd_box(p, (0.0, 0.0, 0.5 * (tip[2] - 0.004)), (0.11, 0.11, 0.5 * (tip[2] - 0.004)), 0.01)
        return union(bed(p), (np.minimum(np.minimum(a, b), printing), M_PART), (hotend(p, tip, 1.5), M_PART2))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.42), extent=1.55, az=12.0, el=8.0)
    o = Overlay(render(scene, cam), cam)
    block_bottom = tip + (0.0, 0.0, 0.13 * 1.5)
    x = 0.36
    dimension(o, tip + (x, -0.2, 0.0), block_bottom + (x, -0.2, 0.0), tick=0.035)
    dashed(o, tip, tip + (x, -0.2, 0.0), PATH, width=1.2, dash=0.03, gap=0.025)
    dashed(o, block_bottom + (0.17 * 1.5, 0.0, 0.0), block_bottom + (x, -0.2, 0.0), PATH, width=1.2, dash=0.03, gap=0.025)
    return o.finish(SIZE)


# --- retraction ------------------------------------------------------------------------------------

TOP_Z = 0.36  # top of the block the loop is printed on


def _loop(inset=0.2, half=0.62):
    h = half - inset
    # counter-clockwise from the front left corner, the seam
    return [np.array(q) for q in ((-h, -h, TOP_Z), (h, -h, TOP_Z), (h, h, TOP_Z), (-h, h, TOP_Z), (-h, -h, TOP_Z))]


def _top_scene(tip=None, scale=1.0):
    def scene(p):
        items = [bed(p), (sd_box(p, (0.0, 0.0, 0.5 * TOP_Z), (0.62, 0.62, 0.5 * TOP_Z), 0.02), M_PART)]
        if tip is not None:
            items.append((hotend(p, tip, scale), M_PART2))
        return union(*items)
    return scene


def wipe_distance(k, n):
    """A loop printed to its seam, then the wipe along its start; then the nozzle lifts and leaves."""
    loop = _loop()
    print_frames, wipe_frames, leave_frames = 10, 5, 3
    wipe_len = 0.6
    t_print = smooth(phase(k, 0, print_frames))
    t_wipe = smooth(phase(k, print_frames, wipe_frames))
    t_leave = smooth(phase(k, print_frames + wipe_frames, leave_frames))
    printed, tip = cut_path(loop, t_print)
    wiped = []
    if t_wipe > 0.0:
        # Wipe::wipe(): from the seam along the loop's first points, for the wipe distance
        whole, _ = cut_path(loop, wipe_len / 3.36)
        wiped, tip = cut_path(whole, t_wipe)
    if t_leave > 0.0:
        tip = tip + np.array([0.25, 0.75, 0.12]) * t_leave
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=1.9, az=25.0, el=48.0)
    o = Overlay(render(_top_scene(tip, 0.8), cam), cam)
    if len(printed) > 1:
        o.polyline(printed, PATH, width=4.2)
    if len(wiped) > 1:
        o.polyline(wiped, TEAL, width=3.0)
    return o.finish(GIF_SIZE)


# --- more shapes and overlay helpers ------------------------------------------------------------


def sd_capsule(p, a, b, r):
    """A rod from a to b, round at the ends."""
    a, b = np.asarray(a, float), np.asarray(b, float)
    ba = b - a
    h = np.clip(((p - a) @ ba) / (ba @ ba), 0.0, 1.0)
    return np.linalg.norm(p - a - h[:, None] * ba, axis=1) - r


def rotated(f, R, centre):
    """The shape f turned by the rotation matrix R about `centre`."""
    centre = np.asarray(centre, float)
    return lambda p: f((p - centre) @ R + centre)


def turn(x, R, centre):
    """A point of a shape turned by `rotated()`, in the world."""
    centre = np.asarray(centre, float)
    return centre + (np.asarray(x, float) - centre) @ R.T


def double_arrow(o, a, b, colour=TEAL_LIGHT, width=2.6, head=8.0):
    a, b = np.asarray(a, float), np.asarray(b, float)
    m = 0.5 * (a + b)
    o.arrow(m, a, colour, width, head)
    o.arrow(m, b, colour, width, head)


def box_edges(o, lo, hi, colour, width=1.4, dashed_lines=False):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    c = [np.array(q, float) for q in ((x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                                         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))]
    for i, j in ((0, 1), (1, 2), (2, 3), (3, 0), (4, 5), (5, 6), (6, 7), (7, 4), (0, 4), (1, 5), (2, 6), (3, 7)):
        if dashed_lines:
            dashed(o, c[i], c[j], colour, width)
        else:
            o.line(c[i], c[j], colour, width)
    return c


def box_volume(o, lo, hi, fill=TEAL + (45,), edge=TEAL_LIGHT, width=1.6):
    """A translucent box: its faces filled, its edges drawn."""
    c = box_edges(o, lo, hi, edge, width)
    for f in ((0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)):
        o.draw.polygon([o.pt(c[i]) for i in f], fill=fill)


def plate_rect(o, x0, y0, x1, y1, fill, outline, width=2.0, z=None):
    z = PLATE_Z + 0.002 if z is None else z
    pts = [np.array(q) for q in ((x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z))]
    fill_polygon(o, pts, fill, outline, width)
    return pts


# --- the bed and the build volume ----------------------------------------------------------------


def printable_height(k, n):
    """The build volume over the plate, its height in ACCENT: nothing may be taller."""
    hgt = 1.6
    tall = lambda p: sd_cylinder(p, (0.2, 0.15), 0.32, PLATE_Z, PLATE_Z + 1.25, 0.02)
    low = lambda p: sd_box(p, (-0.55, -0.35, PLATE_Z + 0.2), (0.25, 0.25, 0.2), 0.02)
    o = _plate_overlay(SIZE, el=24.0, objects=[tall, low], target=(0.0, 0.0, 0.8), extent=3.6)
    z0, z1 = PLATE_Z + 0.002, PLATE_Z + hgt
    box_edges(o, (-1.1, -0.95, z0), (1.1, 0.95, z1), OUTLINE + (150,), 1.2, dashed_lines=True)
    plate_rect(o, -1.1, -0.95, 1.1, 0.95, TEAL + (70,), TEAL_LIGHT, 1.8, z=z1)
    dimension(o, (1.3, -0.95, z0), (1.3, -0.95, z1), normal=(1.0, 0.0, 0.0))
    return o.finish(SIZE)


def bed_exclude_volumes(k, n):
    """Volumes the toolhead must not meet: one from the plate up, one above the plate (a Z range)."""
    objs = [lambda p: sd_box(p, (0.45, -0.3, PLATE_Z + 0.3), (0.25, 0.25, 0.3), 0.02),
            lambda p: sd_box(p, (-0.35, 0.45, PLATE_Z + 0.12), (0.3, 0.22, 0.12), 0.02)]
    o = _plate_overlay(SIZE, el=30.0, objects=objs, target=(0.0, 0.0, 0.45), extent=3.2)
    box_volume(o, (-1.25, -1.1, PLATE_Z), (-0.55, -0.4, PLATE_Z + 0.9))
    box_volume(o, (-0.95, 0.15, PLATE_Z + 0.55), (0.25, 0.85, PLATE_Z + 0.95))
    return o.finish(SIZE)


def _arranged(pos):
    """Objects arranged around a point of the plate, the point marked."""
    x, y = pos
    objs = [lambda p: sd_cylinder(p, (x - 0.32, y + 0.05), 0.24, PLATE_Z, PLATE_Z + 0.3, 0.02),
            lambda p: sd_box(p, (x + 0.27, y - 0.15, PLATE_Z + 0.16), (0.2, 0.2, 0.16), 0.02),
            lambda p: sd_box(p, (x + 0.2, y + 0.38, PLATE_Z + 0.1), (0.24, 0.13, 0.1), 0.02)]
    o = _plate_overlay(SIZE, el=62.0, objects=objs)
    z = PLATE_Z + 0.002
    dashed(o, (-1.25, y, z), (1.25, y, z), TEAL_LIGHT + (170,), 1.2)
    dashed(o, (x, -1.1, z), (x, 1.1, z), TEAL_LIGHT + (170,), 1.2)
    o.dot((x, y, z), 4.5, TEAL_LIGHT, outline=(30, 32, 34))
    return o.finish(SIZE)


def best_object_pos_centre(k, n):
    return _arranged((0.0, 0.0))


def best_object_pos_back(k, n):
    return _arranged((0.0, 0.55))


def preferred_orientation(k, n):
    """An imported model turned about Z by the angle; where it would have stood, dashed."""
    ang = math.radians(35.0)
    R = rot_z(ang)
    c = np.array([0.0, 0.0, 0.0])
    shape = lambda p: np.minimum(sd_box(p, (0.0, -0.25, PLATE_Z + 0.18), (0.55, 0.18, 0.18), 0.02),
                                 sd_box(p, (-0.37, 0.15, PLATE_Z + 0.18), (0.18, 0.4, 0.18), 0.02))
    o = _plate_overlay(SIZE, el=55.0, objects=[rotated(shape, R, c)])
    z = PLATE_Z + 0.004
    ghost = [(-0.55, -0.43), (0.55, -0.43), (0.55, -0.07), (-0.19, -0.07), (-0.19, 0.55), (-0.55, 0.55)]
    o.polyline([np.array((gx, gy, z)) for gx, gy in ghost], OUTLINE + (170,), 1.3, closed=True)
    r = 0.85
    arc = [np.array((r * math.cos(t), r * math.sin(t), z)) for t in np.linspace(-0.55, -0.55 + ang, 20)]
    o.polyline(arc, TEAL_LIGHT, 2.6)
    o.arrow(arc[-3], arc[-1], TEAL_LIGHT, 2.6, 9.0)
    return o.finish(SIZE)


def _tilted_plate(R, ghost_axis):
    """The plate tilted by R about its centre, an object on it; the level plate dashed."""
    c = np.array([0.0, 0.0, 0.3])
    plate_t = rotated(lambda p: sd_box(p, (0.0, 0.0, 0.3 - 0.03), (1.0, 0.85, 0.03), 0.01), R, c)
    obj = rotated(lambda p: sd_box(p, (0.1, 0.0, 0.3 + 0.25), (0.25, 0.25, 0.25), 0.02), R, c)
    scene = lambda p: union(bed(p), (plate_t(p), M_PART2), (obj(p), M_PART))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.45), extent=2.9, az=25.0, el=20.0)
    o = Overlay(render(scene, cam), cam)
    z = 0.3
    o.polyline([np.array(q) for q in ((-1.0, -0.85, z), (1.0, -0.85, z), (1.0, 0.85, z), (-1.0, 0.85, z))],
               OUTLINE + (150,), 1.2, closed=True)
    # the angle, along the front (X tilt) or the right side (Y tilt) of the plate
    edge = np.array(ghost_axis, float)
    base = c + (np.array([0.0, -0.86, 0.0]) if ghost_axis[0] else np.array([1.02, 0.0, 0.0]))
    p_level = base + edge
    p_tilt = turn(p_level, R, c)
    dashed(o, base, p_level, PATH, 1.2)
    o.line(base, p_tilt, TEAL_LIGHT, 2.2)
    arc = []
    for t in np.linspace(0.0, 1.0, 16):
        q = p_level * (1.0 - t) + p_tilt * t - base
        arc.append(base + q / np.linalg.norm(q) * 0.5)
    o.polyline(arc, TEAL_LIGHT, 2.2)
    return o.finish(SIZE)


def plate_tilt_x(k, n):
    return _tilted_plate(rot_y(math.radians(-20.0)), (1.0, 0.0, 0.0))


def plate_tilt_y(k, n):
    return _tilted_plate(rot_x(math.radians(20.0)), (0.0, 0.85, 0.0))


# --- printers ------------------------------------------------------------------------------------


def _post(p, x, y, z0, z1, r=0.05):
    return sd_box(p, (x, y, 0.5 * (z0 + z1)), (r, r, 0.5 * (z1 - z0)), 0.01)


def structure_corexy(k, n):
    """A box frame: the head moves in X and Y at the top, the bed moves down in Z."""
    H, X, Y = 2.3, 1.25, 1.1
    head = np.array([0.25, 0.1, 1.55])

    def scene(p):
        frame = np.minimum.reduce([_post(p, sx * X, sy * Y, 0.0, H) for sx in (-1, 1) for sy in (-1, 1)])
        rails = np.minimum(sd_rod_x(p, -Y, H - 0.08, 0.04, -X, X), sd_rod_x(p, Y, H - 0.08, 0.04, -X, X))
        yrails = np.minimum.reduce([sd_capsule(p, (sx * X, -Y, 1.98), (sx * X, Y, 1.98), 0.04) for sx in (-1, 1)])
        gantry = sd_capsule(p, (-X, head[1], 1.98), (X, head[1], 1.98), 0.04)
        bedp = sd_box(p, (0.0, 0.0, 0.7), (X - 0.12, Y - 0.12, 0.04), 0.01)
        part = sd_box(p, (head[0], head[1], 0.74 + 0.2), (0.2, 0.2, 0.2), 0.02)
        return union(bed(p), (np.minimum.reduce([frame, rails, yrails, gantry]), M_PART2), (bedp, M_PART2),
                     (part, M_PART), (toolhead(p, head - (0.0, 0.0, 0.42)), M_PART2))

    cam = Camera(SIZE, target=(0.0, 0.0, 1.15), extent=3.9, az=28.0, el=22.0)
    o = Overlay(render(scene, cam), cam)
    t = head + (0.0, 0.0, 0.35)
    double_arrow(o, t + (-0.55, 0.0, 0.0), t + (0.55, 0.0, 0.0))
    double_arrow(o, t + (0.0, -0.55, 0.0), t + (0.0, 0.55, 0.0))
    double_arrow(o, (X + 0.25, -Y, 0.45), (X + 0.25, -Y, 1.0))
    return o.finish(SIZE)


def structure_i3(k, n):
    """A bed slinger: the bed moves in Y under a gantry that carries the head in X and Z."""
    head = np.array([0.15, 0.0, 0.85])

    def scene(p):
        posts = np.minimum(_post(p, -1.35, 0.0, 0.0, 2.0, 0.07), _post(p, 1.35, 0.0, 0.0, 2.0, 0.07))
        top = sd_box(p, (0.0, 0.0, 2.0), (1.42, 0.07, 0.07), 0.01)
        xrod = np.minimum(sd_rod_x(p, 0.12, 1.33, 0.035, -1.35, 1.35), sd_rod_x(p, 0.12, 1.18, 0.035, -1.35, 1.35))
        base = np.minimum(sd_rod_x(p, -0.5, 0.1, 0.05, -1.4, 1.4), sd_rod_x(p, 0.5, 0.1, 0.05, -1.4, 1.4))
        bedp = sd_box(p, (0.0, -0.45, 0.3), (0.95, 0.9, 0.04), 0.01)
        part = sd_box(p, (head[0], 0.0, 0.34 + 0.2), (0.22, 0.22, 0.2), 0.02)
        return union(bed(p), (np.minimum.reduce([posts, top, xrod, base]), M_PART2), (bedp, M_PART2), (part, M_PART),
                     (toolhead(p, head - (0.0, 0.0, 0.1)), M_PART2))

    cam = Camera(SIZE, target=(0.0, -0.3, 0.95), extent=3.9, az=32.0, el=22.0)
    o = Overlay(render(scene, cam), cam)
    double_arrow(o, (1.15, -1.25, 0.3), (1.15, 0.35, 0.3))
    double_arrow(o, head + (-0.6, -0.35, 0.75), head + (0.6, -0.35, 0.75), OUTLINE, 2.0)
    return o.finish(SIZE)


def structure_delta(k, n):
    """Three towers; their carriages move up and down and the arms place the head."""
    R, H = 1.25, 2.7
    eff = np.array([0.15, 0.1, 0.75])
    towers = [np.array((R * math.cos(a), R * math.sin(a))) for a in np.radians((-30.0, 90.0, 210.0))]
    heights = [1.85, 2.05, 1.7]

    def scene(p):
        t = np.minimum.reduce([_post(p, x, y, 0.0, H, 0.07) for x, y in towers])
        carr, arms = [], []
        for (x, y), h in zip(towers, heights):
            carr.append(sd_box(p, (x * 0.94, y * 0.94, h), (0.1, 0.1, 0.08), 0.01))
            for side in (-1.0, 1.0):
                d = np.array((-y, x)) / R * 0.08 * side
                arms.append(sd_capsule(p, (x * 0.9 + d[0], y * 0.9 + d[1], h), (eff[0] + d[0] * 0.8, eff[1] + d[1] * 0.8,
                                                                                    eff[2] + 0.18), 0.03))
        bedp = sd_cylinder(p, (0.0, 0.0), 1.05, 0.0, 0.08, 0.01)
        part = sd_cylinder(p, (eff[0], eff[1]), 0.2, 0.08, eff[2] - 0.01, 0.02)
        return union(bed(p), (np.minimum(t, bedp), M_PART2), (np.minimum.reduce(arms), M_PART2),
                     (np.minimum.reduce(carr), M_ACCENT), (part, M_PART), (hotend(p, eff, 0.8), M_PART2))

    cam = Camera(SIZE, target=(0.0, 0.0, 1.25), extent=3.8, az=0.0, el=16.0)
    o = Overlay(render(scene, cam), cam)
    for (x, y), h in zip(towers, heights):
        out = np.array((x, y)) / R * 0.22
        double_arrow(o, (x * 0.94 + out[0], y * 0.94 + out[1], h - 0.38), (x * 0.94 + out[0], y * 0.94 + out[1], h + 0.38),
                     width=2.2, head=7.0)
    return o.finish(SIZE)


def parallel_printheads(k, n):
    """Several printheads on one carriage, each printing a copy of the same object."""
    xs = (-0.8, 0.0, 0.8)
    top = 0.32

    def scene(p):
        objs = [sd_cylinder(p, (x, 0.0), 0.22, PLATE_Z, PLATE_Z + top, 0.02) for x in xs]
        heads = np.minimum.reduce([hotend(p, (x, 0.0, PLATE_Z + top + 0.01)) for x in xs])
        carriage = sd_box(p, (0.0, 0.0, PLATE_Z + top + 0.62), (1.15, 0.2, 0.1), 0.02)
        rod = sd_rod_x(p, 0.3, PLATE_Z + top + 0.66, 0.05, -1.9, 1.9)
        return union(bed(p), (plate(p, PLATE_HALF, PLATE_Z), M_PART2), (objs[0], M_PART), (np.minimum(objs[1], objs[2]), M_ACCENT),
                     (np.minimum.reduce([heads, carriage, rod]), M_PART2))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=3.0, az=22.0, el=26.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


# --- belt printers -------------------------------------------------------------------------------

BELT_Z = 0.25
BELT_HALF = (0.75, 2.0)


def _belt_scene(obj, length=BELT_HALF[1]):
    def scene(p):
        belt = sd_box(p, (0.0, 0.0, BELT_Z - 0.04), (BELT_HALF[0], length, 0.04), 0.01)
        rollers = np.minimum(sd_capsule(p, (-0.8, -length, BELT_Z - 0.13), (0.8, -length, BELT_Z - 0.13), 0.12),
                             sd_capsule(p, (-0.8, length, BELT_Z - 0.13), (0.8, length, BELT_Z - 0.13), 0.12))
        return union(bed(p), (np.minimum(belt, rollers), M_PART2), (obj(p), M_PART))
    return scene


def belt_printer(k, n):
    """A belt for a bed: layers are sliced on a tilted plane and the belt carries the print away."""
    ang = math.radians(45.0)
    y0, y1, h = -0.9, 0.5, 0.75
    obj = lambda p: np.maximum(sd_box(p, (0.0, 0.5 * (y0 + y1), BELT_Z + 0.5 * h), (0.35, 0.5 * (y1 - y0), 0.5 * h), 0.0),
                               (p[:, 1] - y1) * math.sin(ang) + (p[:, 2] - BELT_Z) * math.cos(ang) - h * 0.0)
    cam = Camera(SIZE, target=(0.0, -0.1, 0.55), extent=3.6, az=62.0, el=20.0)
    o = Overlay(render(_belt_scene(obj), cam), cam)
    # the tilted layer planes across the object's side
    for i in range(1, 9):
        yy = y0 + 0.17 * i
        a = np.array((0.36, yy, BELT_Z))
        b = a + np.array((0.0, -math.cos(ang), math.sin(ang))) * 1.2
        if b[2] > BELT_Z + h:
            b = a + np.array((0.0, -math.cos(ang), math.sin(ang))) * (h / math.sin(ang))
        o.line(a, b, TEAL_LIGHT, 1.6)
    base = np.array((0.36, y1 + 0.35, BELT_Z))
    arc = [base + 0.42 * np.array((0.0, -math.cos(t), math.sin(t))) for t in np.linspace(0.0, ang, 14)]
    dashed(o, base, base + (0.0, -0.6, 0.0), PATH, 1.2)
    o.polyline(arc, TEAL_LIGHT, 2.2)
    o.arrow((0.85, -0.6, BELT_Z), (0.85, 0.9, BELT_Z), OUTLINE, 2.2, 9.0)
    return o.finish(SIZE)


def belt_infinite_y(k, n):
    """With no end to Y, an object can be longer than the machine: the belt keeps carrying it away."""
    obj = lambda p: sd_box(p, (0.0, 0.6, BELT_Z + 0.18), (0.25, 2.2, 0.18), 0.02)
    cam = Camera(SIZE, target=(0.0, 0.3, 0.4), extent=4.2, az=55.0, el=26.0)
    o = Overlay(render(_belt_scene(obj, 1.4), cam), cam)
    o.arrow((0.65, -1.2, BELT_Z + 0.05), (0.65, 2.6, BELT_Z + 0.05), TEAL_LIGHT, 2.8, 10.0)
    return o.finish(SIZE)


# --- the rest of Basic information ---------------------------------------------------------------


def extruder_clearance_dist_to_rod(k, n):
    """Seen from the side: from the nozzle across to the far edge of the rod."""
    rod_z = SECOND_H + ROD_ABOVE
    rod_y = 0.38
    rod = lambda p: sd_rod_x(p, rod_y, rod_z + 0.07, 0.07, -1.9, 1.9)
    o, tip = _by_object(SIZE, extra=rod, cam_kw=dict(el=10.0, az=80.0, target=(0.3, 0.1, 0.8), extent=2.6))
    far = np.array([tip[0], rod_y + 0.07, rod_z + 0.07])
    level = np.array([tip[0], 0.0, rod_z + 0.07])
    dashed(o, tip, level + (0.0, 0.0, 0.25), PATH, 1.2)
    dimension(o, level + (0.0, 0.0, 0.25), far + (0.0, 0.0, 0.25), normal=(0.0, 0.0, 1.0))
    dashed(o, far, far + (0.0, 0.0, 0.25), PATH, 1.2)
    return o.finish(SIZE)


MESH = (-0.95, -0.85, 0.95, 0.7)  # the area the probe can reach


def _bed_mesh(corner):
    o = _plate_overlay(SIZE, el=62.0)
    x0, y0, x1, y1 = MESH
    plate_rect(o, x0, y0, x1, y1, TEAL + (50,), TEAL_LIGHT, 1.8)
    z = PLATE_Z + 0.003
    for x in np.linspace(x0, x1, 5):
        for y in np.linspace(y0, y1, 4):
            o.dot((x, y, z), 2.2, OUTLINE, outline=(40, 44, 46))
    cx, cy = (x0, y0) if corner == "min" else (x1, y1)
    ex, ey = (-1.25, -1.1) if corner == "min" else (1.25, 1.1)
    dashed(o, (cx, cy, z), (ex, cy, z), TEAL_LIGHT, 1.2)
    dashed(o, (cx, cy, z), (cx, ey, z), TEAL_LIGHT, 1.2)
    o.dot((cx, cy, z), 5.0, TEAL_LIGHT, outline=(30, 32, 34))
    return o.finish(SIZE)


def bed_mesh_min(k, n):
    return _bed_mesh("min")


def bed_mesh_max(k, n):
    return _bed_mesh("max")


def _probe_grid(step):
    objs = [lambda p: sd_cylinder(p, (-0.15, 0.1), 0.32, PLATE_Z, PLATE_Z + 0.3, 0.02),
            lambda p: sd_box(p, (0.42, -0.2, PLATE_Z + 0.18), (0.18, 0.18, 0.18), 0.02)]
    o = _plate_overlay(SIZE, el=68.0, objects=objs, extent=2.9)
    x0, y0, x1, y1 = -0.75, -0.66, 0.88, 0.7
    plate_rect(o, x0, y0, x1, y1, TEAL + (40,), TEAL_LIGHT, 1.6)
    z = PLATE_Z + 0.003
    for x in np.arange(x0, x1 + 1e-6, step):
        for y in np.arange(y0, y1 + 1e-6, step):
            o.dot((x, y, z), 2.4, TEAL_LIGHT, outline=(30, 32, 34))
    return o.finish(SIZE)


def probe_distance_large(k, n):
    return _probe_grid(0.54)


def probe_distance_small(k, n):
    return _probe_grid(0.27)


def sd_fan(p, centre, facing):
    """A square fan housing with its round grille facing +X or -X."""
    q = p - np.asarray(centre, float)
    housing = sd_box(q, (0.0, 0.0, 0.0), (0.09, 0.32, 0.32), 0.03)
    hub = sd_box(q, (0.1 * facing, 0.0, 0.0), (0.02, 0.1, 0.1), 0.02)
    return np.minimum(housing, hub)


def _aux_fan(sides):
    """The chamber fan on the printer's side wall, blowing across the plate."""
    part = lambda p: np.minimum(sd_cylinder(p, (0.0, 0.0), 0.3, PLATE_Z, PLATE_Z + 0.5, 0.02),
                                sd_box(p, (0.0, 0.0, PLATE_Z + 0.6), (0.45, 0.2, 0.1), 0.02))
    fans = [(lambda p, s=s: sd_fan(p, (1.45 * s, 0.0, 0.6), -s)) for s in sides]
    o_scene = lambda p: union(bed(p), (plate(p, PLATE_HALF, PLATE_Z), M_PART2), (part(p), M_PART),
                              (np.minimum.reduce([f(p) for f in fans]), M_PART2))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.45), extent=3.5, az=12.0, el=22.0)
    o = Overlay(render(o_scene, cam), cam)
    for s in sides:
        for dy, dz in ((-0.15, 0.1), (0.15, -0.05), (0.0, 0.25)):
            a = np.array((1.3 * s, dy, 0.6 + dz))
            o.arrow(a, a + (-0.55 * s, 0.0, -0.05), TEAL_LIGHT, 2.4, 8.0)
    return o.finish(SIZE)


def aux_fan_left(k, n):
    return _aux_fan((-1,))


def aux_fan_right(k, n):
    return _aux_fan((1,))


def aux_fan_both(k, n):
    return _aux_fan((-1, 1))


# --- the extruder page ---------------------------------------------------------------------------


def extruder_printable_area(k, n):
    """The part of the plate this nozzle reaches, in ACCENT, inside the printable area."""
    o = _plate_overlay(SIZE, el=58.0)
    plate_rect(o, -1.1, -0.95, 1.1, 0.95, (0, 0, 0, 0), OUTLINE, 1.4)
    plate_rect(o, -1.1, -0.95, 0.55, 0.95, TEAL + (80,), TEAL_LIGHT, 2.0, z=PLATE_Z + 0.004)
    return o.finish(SIZE)


def extruder_offset(k, n):
    """A second nozzle beside the first: its offset in X and Y from the first one."""
    t0 = np.array([-0.45, -0.1, 0.35])
    t1 = np.array([0.45, 0.2, 0.35])
    scene = lambda p: union(bed(p), (plate(p, PLATE_HALF, PLATE_Z), M_PART2), (hotend(p, t0, 1.3), M_PART2),
                            (hotend(p, t1, 1.3), M_ACCENT))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=2.4, az=20.0, el=30.0)
    o = Overlay(render(scene, cam), cam)
    z = PLATE_Z + 0.002
    a, b = np.array((t0[0], t0[1], z)), np.array((t1[0], t1[1], z))
    dashed(o, t0, a, PATH, 1.2)
    dashed(o, t1, b, PATH, 1.2)
    corner = np.array((t1[0], t0[1], z))
    dimension(o, a, corner, normal=(0.0, 1.0, 0.0), tick=0.04)
    dimension(o, corner, b, normal=(1.0, 0.0, 0.0), tick=0.04)
    return o.finish(SIZE)


def retraction_minimum_travel(k, n):
    """Travels shorter than the threshold (inside the circle) keep the filament; longer ones retract first."""
    z = TOP_Z + 0.001
    r = 0.3
    cam = Camera(SIZE, target=(0.0, 0.0, 0.45), extent=1.9, az=25.0, el=55.0)
    o = Overlay(render(_top_scene(), cam), cam)
    lines = [((-0.45, -0.3), (-0.1, -0.3), (-0.05, -0.12), False), ((-0.45, 0.25), (-0.1, 0.25), (0.42, 0.42), True)]
    for a, p, end, retract in lines:
        a, p, end = np.array((*a, z)), np.array((*p, z)), np.array((*end, z))
        ring = [p + r * np.array((math.cos(t), math.sin(t), 0.0)) for t in np.linspace(0.0, 2.0 * math.pi, 48)]
        o.polyline(ring, TEAL + (200,), 1.4, closed=True)
        o.polyline([a, p], PATH, 4.2)
        dashed(o, p, end, TEAL_LIGHT if retract else TRAVEL_ON_PART, 1.8, dash=0.05, gap=0.04)
        if retract:
            o.dot(p, 5.0, TEAL_LIGHT, outline=TEAL)
    return o.finish(SIZE)


def retract_when_changing_layer(k, n):
    """At the end of a layer: retract, then move up to the next one."""
    loop = _loop()
    seam = loop[0]
    up = seam + np.array([0.0, 0.0, 0.32])
    cam = Camera(SIZE, target=(0.0, 0.0, 0.5), extent=1.9, az=25.0, el=38.0)
    o = Overlay(render(_top_scene(up + (0.0, 0.0, 0.002), 0.8), cam), cam)
    o.polyline(loop, PATH, width=4.2)
    dashed(o, seam, up, PATH, 1.6, dash=0.04, gap=0.03)
    o.dot(seam, 5.5, TEAL_LIGHT, outline=TEAL)
    return o.finish(SIZE)


def z_hop(k, n):
    """A lift before the travel, and its height."""
    path = _hop_path("normal", HOP_A, HOP_B)
    scene = lambda p: union(bed(p), (_hop_blocks(p), M_PART), (hotend(p, HOP_B + (0.0, 0.0, HOP), 0.9), M_PART2))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.75, az=12.0, el=16.0)
    o = Overlay(render(scene, cam), cam)
    o.polyline(path, PATH, width=2.0)
    a = HOP_A + (-0.22, 0.0, 0.0)
    dashed(o, HOP_A, a, PATH, 1.1)
    dashed(o, HOP_A + (0.0, 0.0, HOP), a + (0.0, 0.0, HOP), PATH, 1.1)
    dimension(o, a, a + (0.0, 0.0, HOP))
    o.line(HOP_A, HOP_A + (0.0, 0.0, HOP), TEAL_LIGHT, 2.6)
    return o.finish(SIZE)


# --- machine limits: the axis each one caps -------------------------------------------------------


def _axis(which):
    tip = np.array([0.1, 0.0, 0.42])
    part = lambda p: sd_box(p, (0.1, 0.0, PLATE_Z + 0.18), (0.32, 0.32, 0.18), 0.02)
    feed = lambda p: sd_capsule(p, tip + (0.0, 0.0, 0.85), tip + (0.0, 0.0, 1.9), 0.035)
    scene = lambda p: union(bed(p), (plate(p, PLATE_HALF, PLATE_Z), M_PART2), (part(p), M_PART),
                            (toolhead(p, tip), M_PART2), (feed(p), M_ACCENT if which == "e" else M_PART))
    cam = Camera(SIZE, target=(0.0, 0.0, 0.75), extent=2.9, az=28.0, el=22.0)
    o = Overlay(render(scene, cam), cam)
    c = tip + (0.0, 0.0, 0.62)
    if which == "x":
        double_arrow(o, c + (-0.75, -0.42, 0.0), c + (0.75, -0.42, 0.0), width=3.0, head=10.0)
    elif which == "y":
        double_arrow(o, c + (0.5, -0.8, 0.0), c + (0.5, 0.8, 0.0), width=3.0, head=10.0)
    elif which == "z":
        double_arrow(o, c + (0.55, -0.3, -0.75), c + (0.55, -0.3, 0.55), width=3.0, head=10.0)
    else:
        f = tip + (0.16, -0.1, 1.15)
        double_arrow(o, f, f + (0.0, 0.0, 0.55), width=3.0, head=10.0)
    return o.finish(SIZE)


def axis_x(k, n):
    return _axis("x")


def axis_y(k, n):
    return _axis("y")


def axis_z(k, n):
    return _axis("z")


def axis_e(k, n):
    return _axis("e")


FRAMES = {
    "z_hop_normal": 20,
    "z_hop_slope": 20,
    "z_hop_spiral": 20,
    "z_hop_auto": 22,
    "z_hop_range": 0,
    "retract_lift_all": 0,
    "retract_lift_top": 0,
    "retract_lift_bottom": 0,
    "retract_lift_top_bottom": 0,
    "travel_slope": 0,
    "printable_area": 0,
    "bed_origin": 0,
    "bed_exclude_area": 0,
    "adaptive_bed_mesh_margin": 0,
    "extruder_clearance_radius": 0,
    "extruder_clearance_height_to_rod": 0,
    "extruder_clearance_height_to_lid": 0,
    "nozzle_height": 0,
    "wipe_distance": 20,
}
# The remaining stills.
FRAMES.update({name: 0 for name in (
    "printable_height", "bed_exclude_volumes", "best_object_pos_centre", "best_object_pos_back", "preferred_orientation",
    "plate_tilt_x", "plate_tilt_y", "structure_corexy", "structure_i3", "structure_delta", "parallel_printheads",
    "belt_printer", "belt_infinite_y", "extruder_clearance_dist_to_rod", "bed_mesh_min", "bed_mesh_max",
    "probe_distance_large", "probe_distance_small", "aux_fan_left", "aux_fan_right", "aux_fan_both",
    "extruder_printable_area", "extruder_offset", "retraction_minimum_travel", "retract_when_changing_layer", "z_hop",
    "axis_x", "axis_y", "axis_z", "axis_e")})
