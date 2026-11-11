"""Flat diagrams for the tooltips of the printer settings (SettingTipsPrinter.cpp), beside the ray-marched
pictures of settings_printer.py and into the same folder.

Some settings are about something no plater view shows: the filament inside the hotend, a speed against
time, a fan's response, the zones of an IDEX bed seen from above. Those are drawn here in 2D with PIL,
in the palette of the ray-marched sets: the dark bed as the background, the greys of the parts, and
ACCENT for what the setting changes. No text or numbers; a measured distance is a bracket.

- The hotend cutaways follow the extruder: the filament comes down a tube, through the heat sink and the
  heat break, and melts in the nozzle. A retraction pulls its end up the bore. The single extruder
  multi-material layout adds the cooling tube and the parking position above the hotend
  (WipeTower2::toolchange_Unload / toolchange_Load), and the cutter of a printer that cuts its filament.
- The plots put time on the horizontal axis: a speed profile for the machine limits, a fan's speed for
  the fan settings (GCode/CoolingBuffer, the fan mover), a toolhead's position settling after a corner
  for input shaping, the retracted length through a wipe (Wipe, GCode::retract).
- The IDEX/IQEX views are the bed from above with the front at the bottom, split into the zones
  IMEXZones.cpp computes.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, PART, PART2, SIZE, SS

OUT = "settings/printer"  # resources/images/<OUT>


def _rgb(c, k=1.0):
    return tuple(int(min(255.0, v * k * 255.0 + 0.5)) for v in c)


BG = _rgb(BED, 0.75)  # the background of the ray-marched pictures
GRIDLINE = _rgb(BED, 1.25)
AXIS = (118, 124, 126)
GREY = _rgb(PART)
GREY_DIM = (112, 118, 120)
METAL = _rgb(PART2)
METAL_EDGE = _rgb(PART2, 0.55)
BORE = (22, 23, 24)
FILAMENT = (206, 212, 212)
MELT = (150, 156, 158)
TEAL = _rgb(ACCENT)
TEAL_LIGHT = (70, 205, 185)


def _teal(alpha):
    return TEAL + (alpha,)


class Canvas:
    """A SIZE picture drawn in a 256 x 256 coordinate space, y down, supersampled like the renders."""

    def __init__(self, size=SIZE):
        self.size = size
        self.k = size / 256.0 * SS
        self.image = Image.new("RGB", (size * SS, size * SS), BG)
        self.draw = ImageDraw.Draw(self.image, "RGBA")

    def p(self, x, y):
        return (x * self.k, y * self.k)

    def w(self, width):
        return max(1, int(round(width * self.k)))

    def line(self, pts, colour, width=2.0):
        self.draw.line([self.p(*q) for q in pts], fill=colour, width=self.w(width), joint="curve")

    def poly(self, pts, fill, outline=None, width=1.5):
        self.draw.polygon([self.p(*q) for q in pts], fill=fill)
        if outline is not None:
            self.line(list(pts) + [pts[0]], outline, width)

    def rect(self, x0, y0, x1, y1, fill, outline=None, width=1.5):
        self.poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], fill, outline, width)

    def circle(self, x, y, r, fill, outline=None, width=1.5):
        a, b = self.p(x - r, y - r), self.p(x + r, y + r)
        self.draw.ellipse([a, b], fill=fill, outline=outline, width=self.w(width) if outline else 0)

    def dashed(self, a, b, colour, width=1.4, dash=6.0, gap=4.5):
        a, b = np.asarray(a, float), np.asarray(b, float)
        length = float(np.linalg.norm(b - a))
        if length < 1e-6:
            return
        d = (b - a) / length
        t = 0.0
        while t < length:
            self.line([tuple(a + d * t), tuple(a + d * min(t + dash, length))], colour, width)
            t += dash + gap

    def dashed_path(self, pts, colour, width=1.4, dash=6.0, gap=4.5):
        for a, b in zip(pts[:-1], pts[1:]):
            self.dashed(a, b, colour, width, dash, gap)

    def arrow(self, a, b, colour, width=2.4, head=9.0):
        a, b = np.asarray(a, float), np.asarray(b, float)
        v = b - a
        length = float(np.linalg.norm(v))
        if length < 1e-6:
            return
        v /= length
        n = np.array([-v[1], v[0]])
        base = b - v * head
        self.line([tuple(a), tuple(base)], colour, width)
        self.poly([tuple(b), tuple(base + n * head * 0.45), tuple(base - n * head * 0.45)], colour)

    def dim(self, a, b, colour=TEAL_LIGHT, width=2.0, tick=6.0):
        """A measured distance: the line, and a tick across each end."""
        a, b = np.asarray(a, float), np.asarray(b, float)
        v = (b - a) / max(float(np.linalg.norm(b - a)), 1e-9)
        n = np.array([-v[1], v[0]]) * tick
        self.line([tuple(a), tuple(b)], colour, width)
        for e in (a, b):
            self.line([tuple(e - n), tuple(e + n)], colour, width)

    def finish(self):
        return np.asarray(self.image.resize((self.size, self.size), Image.LANCZOS))


# --- the hotend in section ---------------------------------------------------------------------------
#
# Drawn in its own units, the nozzle's tip at (0, 0) and up as negative y, then scaled and placed. The
# full hotend is 170 units tall.

BORE_R = 4.5
SINK_TOP = -170.0
BREAK = (-100.0, -80.0)  # top, bottom
BLOCK = (-80.0, -40.0)
CONE_TOP = -40.0


def _hotend_parts():
    """The metal of the hotend as polygons, outside the bore, in hotend units."""
    parts = []
    # nozzle cone and its hex
    parts.append([(-22.0, CONE_TOP), (22.0, CONE_TOP), (22.0, -30.0), (7.0, -4.0), (5.0, 0.0), (-5.0, 0.0), (-7.0, -4.0),
                  (-22.0, -30.0)])
    # heater block, longer to the left where the heater cartridge sits
    parts.append([(-60.0, BLOCK[0]), (38.0, BLOCK[0]), (38.0, BLOCK[1]), (-60.0, BLOCK[1])])
    # heat break
    parts.append([(-9.0, BREAK[0]), (9.0, BREAK[0]), (9.0, BREAK[1]), (-9.0, BREAK[1])])
    # heat sink: a core with fins
    parts.append([(-14.0, SINK_TOP), (14.0, SINK_TOP), (14.0, BREAK[0]), (-14.0, BREAK[0])])
    n, span = 9, BREAK[0] - SINK_TOP
    for i in range(n):
        y0 = SINK_TOP + span * i / n
        parts.append([(-36.0, y0), (36.0, y0), (36.0, y0 + span / n * 0.6), (-36.0, y0 + span / n * 0.6)])
    return parts


class Hotend:
    """The hotend cut in half, its nozzle tip at (cx, tip_y), scaled by s."""

    def __init__(self, c, cx=128.0, tip_y=236.0, s=1.2, tube_to=0.0):
        self.c, self.cx, self.tip_y, self.s = c, cx, tip_y, s
        self.tube_to = tube_to  # the top of the PTFE tube above the heat sink, in picture y

    def at(self, x, y):
        return (self.cx + x * self.s, self.tip_y + y * self.s)

    def y(self, u):
        """Picture y of hotend height u (negative is up)."""
        return self.tip_y + u * self.s

    def draw(self):
        c = self.c
        # the PTFE tube above the heat sink
        top = self.tube_to
        c.rect(self.cx - 10.0 * self.s, top, self.cx + 10.0 * self.s, self.y(SINK_TOP), (78, 84, 88), METAL_EDGE, 1.2)
        for poly in _hotend_parts():
            c.poly([self.at(*q) for q in poly], METAL, METAL_EDGE, 1.2)
        # the bore, down to the orifice
        bore = [(-BORE_R, SINK_TOP - 1.0), (BORE_R, SINK_TOP - 1.0), (BORE_R, -14.0), (1.8, -1.0), (1.8, 0.6), (-1.8, 0.6),
                (-1.8, -1.0), (-BORE_R, -14.0)]
        c.poly([self.at(*q) for q in bore], BORE)
        c.rect(self.cx - BORE_R * self.s, top, self.cx + BORE_R * self.s, self.y(SINK_TOP), BORE)

    def filament(self, end_y, melt=True, colour=FILAMENT, tip=None):
        """The filament from the top of the picture down to picture y end_y; molten below it when `melt`."""
        c = self.c
        r = (BORE_R - 0.8) * self.s
        c.rect(self.cx - r, self.tube_to, self.cx + r, end_y, colour)
        if tip is not None:
            c.poly(tip, colour)
        if melt:
            # the melt fills the nozzle from the heat break down to the orifice
            pts = [(-BORE_R + 0.6, BREAK[1]), (BORE_R - 0.6, BREAK[1]), (BORE_R - 0.6, -14.0), (1.4, -1.0), (1.4, 0.4),
                   (-1.4, 0.4), (-1.4, -1.0), (-BORE_R + 0.6, -14.0)]
            c.poly([self.at(*q) for q in pts], MELT)
            if end_y < self.y(BREAK[1]):
                c.rect(self.cx - r, end_y - 1.0, self.cx + r, self.y(BREAK[1]), MELT)

    def cutter(self, y, colour=GREY):
        """A blade across the filament path at picture y, from the right."""
        x = self.cx + 10.0 * self.s
        self.c.poly([(x + 2.0, y - 7.0), (x + 34.0, y - 7.0), (x + 34.0, y + 7.0), (x + 2.0, y + 7.0), (x - 3.0, y)], colour,
                    METAL_EDGE, 1.2)


def _retracted(name_arrow=None):
    c = Canvas()
    h = Hotend(c, tip_y=240.0, s=1.25, tube_to=-2.0)
    h.draw()
    end = h.y(-122.0)  # pulled back out of the melt zone
    h.filament(end, melt=False)
    # the melt left in the nozzle, drawn back from the opening
    pts = [(-BORE_R + 0.6, CONE_TOP), (BORE_R - 0.6, CONE_TOP), (BORE_R - 0.6, -14.0), (2.6, -8.0), (-2.6, -8.0),
           (-BORE_R + 0.6, -14.0)]
    c.poly([h.at(*q) for q in pts], MELT)
    normal_end = h.y(BREAK[1])
    x = h.cx + 52.0 * h.s
    if name_arrow is None:
        c.dashed((h.cx + 5.0 * h.s, normal_end), (x, normal_end), GREY_DIM, 1.2)
        c.dashed((h.cx + 5.0 * h.s, end), (x, end), GREY_DIM, 1.2)
        c.dim((x, normal_end), (x, end))
    else:
        up = name_arrow == "up"
        y0, y1 = (normal_end + 12.0, end - 24.0) if up else (end - 24.0, normal_end + 12.0)
        c.arrow((x, y0), (x, y1), TEAL_LIGHT, width=3.0, head=12.0)
        for dx in (-9.0, 9.0):
            c.line([(x + dx, y0 + (-6.0 if up else 6.0)), (x + dx, (y0 + y1) / 2.0)], _teal(150), 1.6)
    return c.finish()


def retraction_length(k, n):
    """The filament's end pulled up out of the melt zone, by the retraction length."""
    return _retracted()


def retraction_speed(k, n):
    return _retracted("up")


def deretraction_speed(k, n):
    return _retracted("down")


# The single extruder multi-material layout: the hotend small at the bottom, the long tube above it.
SEMM_TIP = 240.0
SEMM_S = 0.62
COOLING_CENTRE = 104.0
COOLING_HALF = 22.0
PARKING = 42.0
CUTTER_Y = 64.0


def _semm(c, cutter=False):
    h = Hotend(c, tip_y=SEMM_TIP, s=SEMM_S, tube_to=-2.0)
    h.draw()
    if cutter:
        h.cutter(CUTTER_Y)
    return h


def _side_dim(c, h, y0, y1, x=None):
    x = h.cx + 46.0 if x is None else x
    for y in (y0, y1):
        c.dashed((h.cx + 8.0, y), (x, y), GREY_DIM, 1.2)
    c.dim((x, y0), (x, y1))


def _cooling_band(c, h, fill, outline):
    c.rect(h.cx - 15.0, COOLING_CENTRE - COOLING_HALF, h.cx + 15.0, COOLING_CENTRE + COOLING_HALF, fill, outline, 1.4)


def cooling_tube_position(k, n):
    c = Canvas()
    h = _semm(c)
    _cooling_band(c, h, (200, 206, 206, 30), GREY_DIM)
    h.filament(COOLING_CENTRE + 6.0, melt=False)
    c.line([(h.cx - 22.0, COOLING_CENTRE), (h.cx + 22.0, COOLING_CENTRE)], TEAL_LIGHT, 2.0)
    _side_dim(c, h, SEMM_TIP, COOLING_CENTRE)
    return c.finish()


def cooling_tube_length(k, n):
    c = Canvas()
    h = _semm(c)
    h.filament(COOLING_CENTRE + 6.0, melt=False)
    _cooling_band(c, h, _teal(80), TEAL_LIGHT)
    _side_dim(c, h, COOLING_CENTRE + COOLING_HALF, COOLING_CENTRE - COOLING_HALF)
    return c.finish()


def parking_position(k, n):
    c = Canvas()
    h = _semm(c)
    _cooling_band(c, h, (200, 206, 206, 30), GREY_DIM)
    h.filament(PARKING, melt=False)
    c.rect(h.cx - 3.7, PARKING - 7.0, h.cx + 3.7, PARKING, TEAL_LIGHT)
    _side_dim(c, h, SEMM_TIP, PARKING)
    return c.finish()


def extra_loading_move(k, n):
    """Loading: down from the parking position by the same distance, plus the extra."""
    c = Canvas()
    h = _semm(c)
    end = SEMM_TIP - 34.0
    h.filament(end, melt=False)
    x = h.cx + 40.0
    c.dashed((h.cx - 30.0, PARKING), (h.cx + 8.0, PARKING), GREY_DIM, 1.2)
    plain = end - 40.0  # where an extra of zero would stop
    c.arrow((x, PARKING), (x, plain + 2.0), GREY, width=2.0, head=8.0)
    c.arrow((x, plain), (x, end), TEAL_LIGHT, width=3.0, head=10.0)
    c.dashed((h.cx + 8.0, plain), (x - 6.0, plain), GREY_DIM, 1.2)
    c.rect(h.cx - 3.7, plain, h.cx + 3.7, end, TEAL_LIGHT)
    return c.finish()


def filament_ramming(k, n):
    """The end of the filament shaped into a clean, tapered tip in the cooling tube before it is parked."""
    c = Canvas()
    h = _semm(c)
    _cooling_band(c, h, (200, 206, 206, 30), GREY_DIM)
    tip_top = COOLING_CENTRE - 10.0
    h.filament(tip_top, melt=False)
    r = BORE_R - 0.8
    tip = [(h.cx - r * SEMM_S - 0.2, tip_top - 0.5), (h.cx + r * SEMM_S + 0.2, tip_top - 0.5), (h.cx + 2.6, tip_top + 10.0),
           (h.cx + 1.0, tip_top + 17.0), (h.cx - 1.0, tip_top + 17.0), (h.cx - 2.6, tip_top + 10.0)]
    c.poly(tip, TEAL_LIGHT)
    return c.finish()


def nozzle_volume(k, n):
    """The filament path from the cutter to the nozzle's end: what a filament change has to flush."""
    c = Canvas()
    h = _semm(c, cutter=True)
    c.rect(h.cx - 3.7, CUTTER_Y, h.cx + 3.7, h.y(BREAK[1]), _teal(230))
    pts = [(-BORE_R + 0.6, BREAK[1]), (BORE_R - 0.6, BREAK[1]), (BORE_R - 0.6, -14.0), (1.4, -1.0), (1.4, 0.4), (-1.4, 0.4),
           (-1.4, -1.0), (-BORE_R + 0.6, -14.0)]
    c.poly([h.at(*q) for q in pts], _teal(230))
    c.rect(h.cx - 3.7, -2.0, h.cx + 3.7, CUTTER_Y - 8.0, FILAMENT)
    _side_dim(c, h, CUTTER_Y, SEMM_TIP, x=h.cx - 46.0)
    return c.finish()


def retraction_when_cut(k, n):
    """A long retraction before the cut: the filament's end drawn far up, so little is left below the cutter."""
    c = Canvas()
    h = _semm(c, cutter=True)
    end = CUTTER_Y + 34.0
    h.filament(end, melt=False)
    _side_dim(c, h, h.y(BREAK[1]), end, x=h.cx - 46.0)
    c.arrow((h.cx + 30.0, end + 30.0), (h.cx + 30.0, end + 4.0), TEAL_LIGHT, width=2.6, head=9.0)
    return c.finish()


def single_extruder_multi_material(k, n):
    """Several filaments fed into one hotend in turn; the one loaded now in ACCENT."""
    c = Canvas()
    h = Hotend(c, cx=128.0, tip_y=244.0, s=0.5, tube_to=128.0)
    h.draw()
    merge = (128.0, 128.0)
    for i, x in enumerate((44.0, 100.0, 156.0, 212.0)):
        active = i == 1
        pts = [(x, 8.0), (x, 52.0), merge]
        c.line(pts, (78, 84, 88), 12.0)
        c.line(pts, TEAL_LIGHT if active else GREY_DIM, 4.0)
        c.circle(x, 20.0, 11.0, (60, 64, 66), TEAL_LIGHT if active else GREY_DIM, 2.0)
    c.circle(merge[0], merge[1], 9.0, METAL, METAL_EDGE, 1.2)
    c.rect(128.0 - 2.4, 132.0, 128.0 + 2.4, h.y(BREAK[1]), TEAL_LIGHT)
    return c.finish()


# --- nozzle tip close-ups --------------------------------------------------------------------------


def _nozzle_tip(c, cx, tip_y, orifice, fill=METAL):
    """The end of a nozzle, large, its flat at tip_y and an opening of half width `orifice`."""
    flat = orifice + 12.0
    c.poly([(cx - 110.0, tip_y - 150.0), (cx + 110.0, tip_y - 150.0), (cx + flat, tip_y), (cx - flat, tip_y)], fill, METAL_EDGE, 1.4)
    c.poly([(cx - orifice - 14.0, tip_y - 150.0), (cx + orifice + 14.0, tip_y - 150.0), (cx + orifice, tip_y - 18.0),
            (cx + orifice, tip_y), (cx - orifice, tip_y), (cx - orifice, tip_y - 18.0)], BORE)


def _bead(c, cx, top, bottom, half, colour):
    """A line's section: flattened, with rounded sides."""
    r = (bottom - top) / 2.0
    c.rect(cx - half + r, top, cx + half - r, bottom, colour)
    c.circle(cx - half + r, top + r, r, colour)
    c.circle(cx + half - r, top + r, r, colour)


BED_Y = 214.0


def _bed_line(c):
    c.rect(0.0, BED_Y, 256.0, 256.0, (52, 55, 57))
    c.line([(0.0, BED_Y), (256.0, BED_Y)], (88, 94, 96), 1.4)


def _nozzle_diameter(orifice):
    c = Canvas()
    _bed_line(c)
    layer = 18.0 + orifice * 0.6
    _bead(c, 128.0, BED_Y - layer, BED_Y, orifice * 1.25 + layer * 0.35, TEAL_LIGHT)
    # the molten line coming out of the opening
    c.rect(128.0 - orifice, BED_Y - layer - 26.0, 128.0 + orifice, BED_Y - layer + 2.0, TEAL_LIGHT)
    _nozzle_tip(c, 128.0, BED_Y - layer, orifice)
    return c.finish()


def nozzle_diameter_small(k, n):
    return _nozzle_diameter(11.0)


def nozzle_diameter_large(k, n):
    return _nozzle_diameter(22.0)


def _z_offset(shift):
    """The nozzle moved by the Z offset from where the G-code puts it; the first layer squashed or not."""
    c = Canvas()
    _bed_line(c)
    nominal = BED_Y - 26.0
    tip = nominal - shift
    gap = BED_Y - tip
    half = 30.0 * 26.0 / gap
    _bead(c, 128.0, tip, BED_Y, max(half, gap / 2.0 + 1.0), GREY)
    _nozzle_tip(c, 128.0, tip, 9.0)
    x = 200.0
    c.dashed((40.0, nominal), (x + 10.0, nominal), GREY_DIM, 1.4)
    if abs(shift) > 1.0:
        c.arrow((x, nominal), (x, tip + (-2.0 if shift > 0 else 2.0)), TEAL_LIGHT, width=2.8, head=9.0)
    return c.finish()


def z_offset_up(k, n):
    return _z_offset(14.0)


def z_offset_down(k, n):
    return _z_offset(-12.0)


# --- plots against time ------------------------------------------------------------------------------

PX0, PX1, PY0, PY1 = 26.0, 232.0, 40.0, 212.0  # the plot area: x is time, y up is the quantity


def _plot(c, grid=True):
    if grid:
        for i in range(1, 5):
            y = PY1 - (PY1 - PY0) * i / 4.0
            c.line([(PX0, y), (PX1, y)], GRIDLINE, 1.0)
    c.line([(PX0, PY0 - 6.0), (PX0, PY1), (PX1 + 6.0, PY1)], AXIS, 1.6)


def _xy(t, v):
    """Plot coordinates of time t and value v, both 0..1."""
    return (PX0 + (PX1 - PX0) * t, PY1 - (PY1 - PY0) * v)


def _curve(c, ts, vs, colour, width=2.6, dashed=False):
    pts = [_xy(t, v) for t, v in zip(ts, vs)]
    if dashed:
        c.dashed_path(pts, colour, width)
    else:
        c.line(pts, colour, width)


def _trapezoid(t0, t1, v, a, ts):
    """A move from t0 to t1 accelerating at `a` (per unit time) up to v and back down: its speed at ts."""
    out = []
    for t in ts:
        if t < t0 or t > t1:
            out.append(0.0)
        else:
            out.append(min(v, a * (t - t0), a * (t1 - t)))
    return out


def limit_speed(k, n):
    """The speed the move asks for (dashed) and the one the limit allows: capped, and so a longer move."""
    c = Canvas()
    _plot(c)
    ts = np.linspace(0.0, 1.0, 400)
    _curve(c, ts, _trapezoid(0.05, 0.75, 0.9, 4.0, ts), GREY, 2.0, dashed=True)
    cap = 0.55
    # the same distance at the lower speed takes longer
    area = 0.9 * (0.70 - 0.9 / 4.0)
    dur = area / cap + cap / 4.0
    _curve(c, ts, _trapezoid(0.05, 0.05 + dur, cap, 4.0, ts), TEAL_LIGHT, 3.0)
    c.dashed(_xy(0.0, cap), _xy(1.0, cap), _teal(200), 1.4)
    return c.finish()


def limit_accel(k, n):
    """Ramps up and down no steeper than the acceleration limit."""
    c = Canvas()
    _plot(c)
    ts = np.linspace(0.0, 1.0, 400)
    _curve(c, ts, _trapezoid(0.06, 0.62, 0.75, 9.0, ts), GREY, 2.0, dashed=True)
    _curve(c, ts, _trapezoid(0.06, 0.06 + 0.56 + 0.75 / 2.6 - 0.75 / 9.0, 0.75, 2.6, ts), TEAL_LIGHT, 3.0)
    # the slope, marked on the ramp up
    a = _xy(0.06, 0.0)
    r = 40.0
    ang = math.atan2((PY1 - PY0) * 2.6, (PX1 - PX0))
    arc = [(a[0] + r * math.cos(-t), a[1] + r * math.sin(-t)) for t in np.linspace(0.0, ang, 20)]
    c.line(arc, TEAL, 2.0)
    return c.finish()


def limit_jerk(k, n):
    """The speed change a move may start, stop or turn with at once, before it has to accelerate."""
    c = Canvas()
    _plot(c)
    j = 0.22
    t0, t1, v, a = 0.08, 0.86, 0.72, 3.0
    pts = [(t0, 0.0), (t0, j), (t0 + (v - j) / a, v), (t1 - (v - j) / a, v), (t1, j), (t1, 0.0)]
    _curve(c, [p[0] for p in pts[1:5]], [p[1] for p in pts[1:5]], GREY, 2.6)
    for a_, b_ in ((pts[0], pts[1]), (pts[4], pts[5])):
        c.line([_xy(*a_), _xy(*b_)], TEAL_LIGHT, 3.6)
    c.dashed(_xy(0.0, j), _xy(1.0, j), _teal(170), 1.3)
    return c.finish()


def junction_deviation(k, n):
    """A corner seen from above: how far inside the sharp corner the planner lets the path round off."""
    c = Canvas()
    corner = (186.0, 190.0)
    c.line([(28.0, 190.0), corner, (186.0, 30.0)], GREY, 4.0)
    r = 64.0
    centre = (corner[0] - r, corner[1] - r)
    arc = [(centre[0] + r * math.cos(t), centre[1] + r * math.sin(t)) for t in np.linspace(0.0, math.pi / 2.0, 30)]
    c.line(arc, TEAL_LIGHT, 3.4)
    mid = (centre[0] + r * math.cos(math.pi / 4.0), centre[1] + r * math.sin(math.pi / 4.0))
    c.dim(corner, mid, TEAL_LIGHT, 2.0, tick=5.0)
    c.arrow((40.0, 210.0), (100.0, 210.0), GREY_DIM, 2.0, 8.0)
    return c.finish()


def fan_speedup_time(k, n):
    """The fan command moved earlier, so the fan is already at speed when the overhang starts."""
    c = Canvas()
    _plot(c)
    start, end, target = 0.58, 0.84, 0.8
    lead = 0.2
    # the overhang along the time axis
    c.line([_xy(0.0, -0.06), _xy(start, -0.06)], GREY_DIM, 5.0)
    c.line([_xy(start, -0.06), _xy(end, -0.06)], GREY, 5.0)
    c.line([_xy(end, -0.06), _xy(1.0, -0.06)], GREY_DIM, 5.0)
    c.dashed(_xy(start, 0.0), _xy(start, 1.05), GREY_DIM, 1.3)
    ts = np.linspace(0.0, 1.0, 300)

    def fan(t_on):
        out = []
        for t in ts:
            if t < t_on:
                out.append(0.12)
            elif t < end:
                out.append(0.12 + (target - 0.12) * (1.0 - math.exp(-(t - t_on) / 0.055)))
            else:
                out.append(0.12 + (target - 0.12) * math.exp(-(t - end) / 0.05))
        return out

    _curve(c, ts, fan(start), GREY, 2.0, dashed=True)
    _curve(c, ts, fan(start - lead), TEAL_LIGHT, 3.0)
    y = _xy(0.0, 1.0)[1]
    c.dim(_xy(start - lead, 1.0), _xy(start, 1.0), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


def fan_kickstart(k, n):
    """Full speed for a moment to get the fan spinning, then down to the speed asked for."""
    c = Canvas()
    _plot(c)
    on, kick, off, target = 0.22, 0.14, 0.86, 0.38
    c.dashed(_xy(0.0, target), _xy(1.0, target), GREY_DIM, 1.3)
    _curve(c, [0.0, on], [0.0, 0.0], GREY, 2.6)
    _curve(c, [on, on, on + kick, on + kick], [0.0, 1.0, 1.0, target], TEAL_LIGHT, 3.2)
    _curve(c, [on + kick, off, off, 1.0], [target, target, 0.0, 0.0], GREY, 2.6)
    c.dim(_xy(on, 1.08), _xy(on + kick, 1.08), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


def fan_min_pwm(k, n):
    """Fan commands as bars: those above zero but under the floor are raised to it; zero stays off."""
    c = Canvas()
    _plot(c)
    floor = 0.3
    values = [0.0, 0.12, 0.55, 0.2, 0.0, 0.85, 0.08, 0.4]
    w = (PX1 - PX0) / len(values)
    for i, v in enumerate(values):
        x0 = PX0 + w * i + w * 0.2
        x1 = PX0 + w * (i + 1) - w * 0.2
        if v <= 0.0:
            c.line([(x0, PY1 - 1.0), (x1, PY1 - 1.0)], GREY_DIM, 2.0)
            continue
        c.rect(x0, _xy(0, v)[1], x1, PY1, GREY)
        if v < floor:
            c.rect(x0, _xy(0, floor)[1], x1, _xy(0, v)[1], TEAL_LIGHT)
    c.dashed(_xy(0.0, floor), _xy(1.0, floor), TEAL_LIGHT, 1.6)
    return c.finish()


def resonance_avoidance(k, n):
    """Speeds on a line: an outer wall speed inside the resonant band moves out to its nearer end."""
    c = Canvas()
    lo, hi = 0.40, 0.66
    x0, x1 = 24.0, 232.0
    sx = lambda v: x0 + (x1 - x0) * v
    c.rect(sx(lo), 40.0, sx(hi), 216.0, _teal(60))
    for x in (sx(lo), sx(hi)):
        c.line([(x, 40.0), (x, 216.0)], TEAL_LIGHT, 1.8)
    rows = [(0.47, lo), (0.60, hi), (0.22, None), (0.84, None)]
    for i, (v, to) in enumerate(rows):
        y = 64.0 + i * 42.0
        c.line([(x0, y), (x1, y)], GRIDLINE, 1.4)
        if to is None:
            c.circle(sx(v), y, 6.5, GREY)
            continue
        c.circle(sx(v), y, 6.5, None, GREY_DIM, 1.6)
        c.arrow((sx(v) + (-8.0 if to < v else 8.0), y), (sx(to) + (2.0 if to < v else -2.0), y), TEAL_LIGHT, 2.4, 8.0)
        c.circle(sx(to), y, 6.5, TEAL_LIGHT)
    c.arrow((x0, 232.0), (x1, 232.0), AXIS, 1.6, 8.0)
    return c.finish()


def input_shaping(k, n):
    """The toolhead's position after a sharp change of direction: ringing on without shaping, settled with it."""
    c = Canvas()
    _plot(c)
    ts = np.linspace(0.0, 1.0, 500)
    t0 = 0.12
    ramp = lambda t: min(max((t - t0) / 0.16, 0.0), 1.0) * 0.62 + 0.12
    c.dashed(_xy(0.0, 0.74), _xy(1.0, 0.74), GREY_DIM, 1.3)
    ring = [ramp(t) + (0.17 * math.exp(-(t - t0 - 0.16) / 0.22) * math.sin(2 * math.pi * (t - t0 - 0.16) / 0.11)
                       if t > t0 + 0.16 else 0.0) for t in ts]
    _curve(c, ts, ring, GREY, 2.0)
    shaped = []
    for t in ts:
        u = min(max((t - t0) / 0.2, 0.0), 1.0)
        shaped.append(0.12 + 0.62 * (u * u * (3.0 - 2.0 * u)))
    _curve(c, ts, shaped, TEAL_LIGHT, 3.0)
    return c.finish()


def _wipe_retraction(highlight):
    """Retracted length through a wipe: a quick pull before it, a slow one while it moves, the rest after."""
    c = Canvas()
    _plot(c)
    before, during, after = 0.35, 0.3, 0.35
    t = [0.06, 0.16, 0.66, 0.76]
    v = [0.0, before, before + during, 1.0]
    c.rect(_xy(t[1], 0)[0], PY1 + 8.0, _xy(t[2], 0)[0], PY1 + 15.0, GREY)  # the nozzle moving: the wipe
    _curve(c, [0.0, t[0]], [0.0, 0.0], GREY, 2.6)
    segs = [("before", t[0], t[1], v[0], v[1]), ("wipe", t[1], t[2], v[1], v[2]), ("after", t[2], t[3], v[2], v[3])]
    for name, ta, tb, va, vb in segs:
        on = name == highlight
        _curve(c, [ta, tb], [va, vb], TEAL_LIGHT if on else GREY, 3.4 if on else 2.4)
    _curve(c, [t[3], 1.0], [1.0, 1.0], GREY, 2.6)
    for name, ta, tb, va, vb in segs:
        if name == highlight:
            x = _xy(tb, 0)[0] + 12.0 if name == "before" else _xy(ta, 0)[0] - 12.0
            c.dim((x, _xy(0, va)[1]), (x, _xy(0, vb)[1]), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


def retract_before_wipe(k, n):
    return _wipe_retraction("before")


def retract_after_wipe(k, n):
    return _wipe_retraction("after")


def _dome_layers(hmin, hmax):
    """Adaptive layers over a dome: thick on its steep sides, thin where it flattens out at the top."""
    R = 150.0
    z, out = 0.0, []
    while z < R - 1.0:
        nz = z / R
        h = hmax - (hmax - hmin) * nz ** 1.6
        h = min(h, R - z)
        out.append((z, z + h))
        z += h
    return R, out


def _layer_height(which):
    c = Canvas()
    hmin, hmax = 4.0, 21.0
    R, layers = _dome_layers(hmin, hmax)
    base = 226.0
    cx = 128.0
    for z0, z1 in layers:
        # a layer prints the dome's section at its top (stair-stepped, as sliced)
        half = math.sqrt(max(R * R - z1 * z1, 0.0)) * 0.72
        h = z1 - z0
        on = (which == "min" and h < hmin * 1.7) or (which == "max" and h > hmax * 0.9)
        colour = TEAL_LIGHT if on else GREY
        c.rect(cx - half, base - z1 * 1.0 + 0.6, cx + half, base - z0 * 1.0 - 0.6, colour)
    # the smooth shape being approximated
    pts = [(cx + R * 0.72 * math.cos(t), base - R * math.sin(t)) for t in np.linspace(0.0, math.pi, 80)]
    c.line(pts, GREY_DIM, 1.2)
    c.line([(10.0, base + 1.0), (246.0, base + 1.0)], (88, 94, 96), 1.4)
    return c.finish()


def layer_height_min(k, n):
    return _layer_height("min")


def layer_height_max(k, n):
    return _layer_height("max")


# --- IDEX / IQEX zones, from above, front at the bottom -------------------------------------------

BX0, BX1, BY0, BY1 = 28.0, 228.0, 52.0, 212.0


def _bed2d(c):
    c.rect(BX0 - 6.0, BY0 - 6.0, BX1 + 6.0, BY1 + 6.0, (52, 55, 57), (88, 94, 96), 1.4)


def _toolhead2d(c, x, y, colour=METAL, outline=METAL_EDGE, w=30.0, h=24.0):
    c.rect(x - w / 2.0, y - h / 2.0, x + w / 2.0, y + h / 2.0, colour, outline, 1.4)
    c.circle(x, y, 3.5, BORE)


def _zones(cols, rows, primary=None, gantry_teal=False):
    """The bed split into cols x rows zones, one toolhead parked in each; `primary` (col, row) in ACCENT."""
    c = Canvas()
    _bed2d(c)
    zw, zh = (BX1 - BX0) / cols, (BY1 - BY0) / rows
    for i in range(cols):
        for j in range(rows):
            x0, y1 = BX0 + zw * i, BY1 - zh * j  # row 0 is the front, at the bottom
            if primary == (i, j):
                c.rect(x0 + 2.0, y1 - zh + 2.0, x0 + zw - 2.0, y1 - 2.0, _teal(70), TEAL_LIGHT, 1.6)
    for i in range(1, cols):
        c.dashed((BX0 + zw * i, BY0), (BX0 + zw * i, BY1), GREY, 1.6)
    for j in range(1, rows):
        c.dashed((BX0, BY0 + zh * j), (BX1, BY0 + zh * j), GREY, 1.6)
    for j in range(rows):
        y = BY1 - zh * (j + 0.5)
        c.line([(BX0 - 14.0, y), (BX1 + 14.0, y)], TEAL_LIGHT if gantry_teal else (96, 102, 106), 4.0)
        for i in range(cols):
            x = BX0 + zw * (i + 0.5)
            on = primary == (i, j)
            _toolhead2d(c, x, y, TEAL if on else METAL, TEAL_LIGHT if on else METAL_EDGE)
    return c


def imex_idex(k, n):
    return _zones(2, 1, gantry_teal=True).finish()


def imex_iqex(k, n):
    return _zones(2, 2, gantry_teal=True).finish()


def imex_t0_front_left(k, n):
    return _zones(2, 2, (0, 0)).finish()


def imex_t0_front_right(k, n):
    return _zones(2, 2, (1, 0)).finish()


def imex_t0_rear_left(k, n):
    return _zones(2, 2, (0, 1)).finish()


def imex_t0_rear_right(k, n):
    return _zones(2, 2, (1, 1)).finish()


def _hatch(c, x0, y0, x1, y1, colour, step=9.0, width=1.4):
    """Diagonal hatching clipped to a rectangle."""
    t = -(y1 - y0)
    while t < (x1 - x0):
        a = (x0 + t, y1)
        b = (x0 + t + (y1 - y0), y0)
        # clip to the rectangle along the 45 degree line
        s0 = max(0.0, (x0 - a[0]) / (b[0] - a[0]))
        s1 = min(1.0, (x1 - a[0]) / (b[0] - a[0]))
        if s1 > s0:
            pa = (a[0] + (b[0] - a[0]) * s0, a[1] + (b[1] - a[1]) * s0)
            pb = (a[0] + (b[0] - a[0]) * s1, a[1] + (b[1] - a[1]) * s1)
            c.line([pa, pb], colour, width)
        t += step


def imex_clearance_x(k, n):
    """The strip along the X boundary of the primary zone that the other carriage could reach into."""
    c = Canvas()
    _bed2d(c)
    mid = (BX0 + BX1) / 2.0
    strip = 26.0
    c.rect(mid - strip, BY0, mid, BY1, _teal(60), None)
    _hatch(c, mid - strip, BY0, mid, BY1, _teal(200))
    c.dashed((mid, BY0), (mid, BY1), GREY, 1.6)
    y = (BY0 + BY1) / 2.0
    c.line([(BX0 - 14.0, y), (BX1 + 14.0, y)], (96, 102, 106), 4.0)
    # the primary carriage with its nozzle at the strip's inner edge: its body reaches the boundary
    nz = mid - strip
    _toolhead2d(c, nz, y, w=2.0 * strip, h=30.0)
    _toolhead2d(c, mid + 60.0, y, w=2.0 * strip, h=30.0)
    c.dim((nz, y - 30.0), (mid, y - 30.0), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


def imex_clearance_y(k, n):
    c = Canvas()
    _bed2d(c)
    mid = (BY0 + BY1) / 2.0
    strip = 22.0
    c.rect(BX0, mid, BX1, mid + strip, _teal(60), None)
    _hatch(c, BX0, mid, BX1, mid + strip, _teal(200))
    c.dashed((BX0, mid), (BX1, mid), GREY, 1.6)
    nz = mid + strip  # the primary (front) carriage's nozzle, at the strip's inner edge
    for y in (nz, mid - 50.0):
        c.line([(BX0 - 14.0, y), (BX1 + 14.0, y)], (96, 102, 106), 4.0)
    x = (BX0 + BX1) / 2.0 - 40.0
    _toolhead2d(c, x, nz, w=34.0, h=2.0 * strip)
    _toolhead2d(c, x + 70.0, mid - 50.0, w=34.0, h=2.0 * strip)
    c.dim((x + 32.0, nz), (x + 32.0, mid), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


def imex_carriage_margin(k, n):
    """A band inside the primary zone, before the collision strip: a reminder to leave room, not a limit."""
    c = Canvas()
    _bed2d(c)
    mid = (BX0 + BX1) / 2.0
    strip, margin = 22.0, 22.0
    c.rect(mid - strip, BY0, mid, BY1, (200, 206, 206, 30), None)
    _hatch(c, mid - strip, BY0, mid, BY1, GREY_DIM)
    c.rect(mid - strip - margin, BY0, mid - strip, BY1, _teal(90), None)
    c.line([(mid - strip - margin, BY0), (mid - strip - margin, BY1)], TEAL_LIGHT, 1.6)
    c.dashed((mid, BY0), (mid, BY1), GREY, 1.6)
    y = (BY0 + BY1) / 2.0
    c.dim((mid - strip - margin, y), (mid - strip, y), TEAL_LIGHT, 2.0, tick=5.0)
    return c.finish()


FRAMES = {
    "retraction_length": 0,
    "retraction_speed": 0,
    "deretraction_speed": 0,
    "cooling_tube_position": 0,
    "cooling_tube_length": 0,
    "parking_position": 0,
    "extra_loading_move": 0,
    "filament_ramming": 0,
    "nozzle_volume": 0,
    "retraction_when_cut": 0,
    "single_extruder_multi_material": 0,
    "nozzle_diameter_small": 0,
    "nozzle_diameter_large": 0,
    "z_offset_up": 0,
    "z_offset_down": 0,
    "limit_speed": 0,
    "limit_accel": 0,
    "limit_jerk": 0,
    "junction_deviation": 0,
    "fan_speedup_time": 0,
    "fan_kickstart": 0,
    "fan_min_pwm": 0,
    "resonance_avoidance": 0,
    "input_shaping": 0,
    "retract_before_wipe": 0,
    "retract_after_wipe": 0,
    "layer_height_min": 0,
    "layer_height_max": 0,
    "imex_idex": 0,
    "imex_iqex": 0,
    "imex_t0_front_left": 0,
    "imex_t0_front_right": 0,
    "imex_t0_rear_left": 0,
    "imex_t0_rear_right": 0,
    "imex_clearance_x": 0,
    "imex_clearance_y": 0,
    "imex_carriage_margin": 0,
}
