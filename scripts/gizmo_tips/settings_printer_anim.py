"""Animated pictures for the printer settings whose point is a movement (SettingTipsPrinter.cpp), into the
folder of settings_printer.py and settings_printer_more.py, with their scenes, camera and palette.

- Retraction: the filament's end pulled up the bore out of the melt and pushed back down. Each of the
  three settings has the same loop; ACCENT marks the part it sets: the distance for the length, the move
  up for the retraction speed, the move down for the deretraction speed.
- Extra length on restart: a line, the travel, and the extra filament pushed out at the restart before
  the next line, in that order (GCode::unretract after the travel).
- Long retraction when cut: the filament drawn far up the tube first, then cut, so only a short piece is
  left below the cutter for the next filament to push out.
"""

import numpy as np

from .render import GIF_SIZE, Camera, Overlay, phase, render, smooth
from .settings_printer import PATH, TOP_Z, TRAVEL_ON_PART, _top_scene, cut_path, dashed
from .settings_printer_more import (BORE_R, BREAK, CONE_TOP, CUTTER_Y, FILAMENT, GREY, GREY_DIM, MELT, METAL_EDGE,
                                    SEMM_S, SEMM_TIP, TEAL, TEAL_LIGHT, Canvas, Hotend)

OUT = "settings/printer"  # resources/images/<OUT>

MARK = (126, 132, 134)  # the marks across the filament, so a move of it shows


def _lerp(a, b, t):
    return a + (b - a) * t


def _marks(c, h, top, end, travel, step=14.0):
    """Lines across the filament from picture y `top` down to `end`, moving with it by `travel`."""
    r = (BORE_R - 0.8) * h.s
    y = top + (travel % step)
    while y < end - 3.0:
        if y > top + 1.0:
            c.line([(h.cx - r, y), (h.cx + r, y)], MARK, 1.5)
        y += step


def _melt(c, h, t):
    """The melt in the nozzle: filling it from the heat break at t = 0, drawn back into the cone at t = 1."""
    top = _lerp(BREAK[1], CONE_TOP, t)
    rx, yb = _lerp(1.4, 2.6, t), _lerp(0.4, -8.0, t)
    pts = [(-BORE_R + 0.6, top), (BORE_R - 0.6, top), (BORE_R - 0.6, -14.0), (rx, yb), (-rx, yb), (-BORE_R + 0.6, -14.0)]
    c.poly([h.at(*q) for q in pts], MELT)


# --- retraction length, retraction speed, deretraction speed -----------------------------------------
#
# Rest, pulled up, held, pushed back down, rest; 20 frames.

def _retraction(k, which):
    if k <= 2:
        t, moving = 0.0, None
    elif k <= 8:
        t, moving = smooth(phase(k, 3, 6)), "up"
    elif k <= 12:
        t, moving = 1.0, None
    elif k <= 17:
        t, moving = 1.0 - smooth(phase(k, 13, 5)), "down"
    else:
        t, moving = 0.0, None

    c = Canvas(GIF_SIZE)
    h = Hotend(c, tip_y=240.0, s=1.25, tube_to=-2.0)
    h.draw()
    normal_end, far_end = h.y(BREAK[1]), h.y(-122.0)
    end = _lerp(normal_end, far_end, t)
    h.filament(end, melt=False)
    _marks(c, h, h.tube_to, end, end - normal_end)
    _melt(c, h, t)

    x = h.cx + 52.0 * h.s
    if which == "length":
        c.dashed((h.cx + 5.0 * h.s, normal_end), (x, normal_end), GREY_DIM, 1.2)
        if t > 0.03:
            c.dashed((h.cx + 5.0 * h.s, end), (x, end), GREY_DIM, 1.2)
            c.dim((x, normal_end), (x, end))
    elif moving is not None:
        # The move this setting sets in ACCENT, the other one in grey.
        up = moving == "up"
        y0, y1 = (normal_end + 12.0, far_end - 24.0) if up else (far_end - 24.0, normal_end + 12.0)
        mine = (which == "speed") == up
        colour = TEAL_LIGHT if mine else GREY_DIM
        c.arrow((x, y0), (x, y1), colour, width=3.0 if mine else 2.0, head=12.0 if mine else 9.0)
        if mine:
            for dx in (-9.0, 9.0):
                c.line([(x + dx, y0 + (-6.0 if up else 6.0)), (x + dx, (y0 + y1) / 2.0)], TEAL + (150,), 1.6)
    return c.finish()


def retraction_length(k, n):
    return _retraction(k, "length")


def retraction_speed(k, n):
    return _retraction(k, "speed")


def deretraction_speed(k, n):
    return _retraction(k, "deretraction")


# --- extra length on restart -------------------------------------------------------------------------


def retract_restart_extra(k, n):
    """A line, the travel to the next one, the extra pushed out at the restart, then the next line."""
    z = TOP_Z
    first = [np.array(q) for q in ((-0.42, -0.32, z), (0.12, -0.32, z))]
    restart = np.array([-0.3, 0.25, z])
    second = [restart, np.array([0.12, 0.25, z])]
    t_first = smooth(phase(k, 0, 6))
    t_travel = smooth(phase(k, 6, 5))
    t_extra = phase(k, 11, 3)
    t_second = smooth(phase(k, 14, 5))

    printed, tip = cut_path(first, t_first)
    travelled = []
    if t_travel > 0.0:
        travelled, tip = cut_path([first[-1], restart], t_travel)
    if t_second > 0.0:
        _, tip = cut_path(second, t_second)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=1.9, az=25.0, el=48.0)
    o = Overlay(render(_top_scene(tip, 0.8), cam), cam)
    o.polyline(printed, PATH, width=4.2)
    if len(travelled) > 1:
        dashed(o, travelled[0], travelled[-1], TRAVEL_ON_PART, width=1.6, dash=0.05, gap=0.04)
    if t_second > 0.0:
        o.polyline(cut_path(second, t_second)[0], PATH, width=4.2)
    if t_extra > 0.0:
        o.dot(restart, 2.0 + 4.5 * t_extra, TEAL_LIGHT, outline=TEAL)
    return o.finish(GIF_SIZE)


# --- long retraction when cut ------------------------------------------------------------------------


def retraction_when_cut(k, n):
    """Pulled far up, cut, and the filament above the cutter taken away; a short piece stays below it."""
    c = Canvas(GIF_SIZE)
    h = Hotend(c, tip_y=SEMM_TIP, s=SEMM_S, tube_to=-2.0)
    h.draw()
    normal_end, far_end = h.y(BREAK[1]), CUTTER_Y + 34.0
    t = smooth(phase(k, 3, 7))  # the long retraction, frames 3..9
    blade = smooth(phase(k, 10, 3)) - smooth(phase(k, 15, 3))  # in over 10..12, out over 15..17
    away = smooth(phase(k, 14, 5))  # the cut filament unloaded upwards, 14..18
    end = _lerp(normal_end, far_end, t)
    r = (BORE_R - 0.8) * h.s

    if k < 14:
        h.filament(end, melt=False)
        _marks(c, h, h.tube_to, end, end - normal_end, step=9.0)
    else:
        # Cut: the piece below the cutter stays, the rest goes up the tube.
        gap = 2.0 + 44.0 * away
        c.rect(h.cx - r, h.tube_to, h.cx + r, CUTTER_Y - gap, FILAMENT)
        _marks(c, h, h.tube_to, CUTTER_Y - gap, far_end - normal_end - gap, step=9.0)
        c.rect(h.cx - r, CUTTER_Y + 1.0, h.cx + r, far_end, FILAMENT)
    _melt(c, h, t)

    # The blade, sliding in from the right across the filament and back.
    x = h.cx + 10.0 * h.s + 16.0 - 26.0 * blade
    y = CUTTER_Y
    c.poly([(x + 2.0, y - 7.0), (x + 34.0, y - 7.0), (x + 34.0, y + 7.0), (x + 2.0, y + 7.0), (x - 3.0, y)], GREY,
           METAL_EDGE, 1.2)

    # The retraction distance, measured from where the filament ends in the hotend.
    xd = h.cx - 46.0
    c.dashed((h.cx - 8.0, normal_end), (xd, normal_end), GREY_DIM, 1.2)
    if t > 0.03:
        c.dashed((h.cx - 8.0, end), (xd, end), GREY_DIM, 1.2)
        c.dim((xd, normal_end), (xd, end))
    if 3 <= k <= 9:
        c.arrow((h.cx + 30.0, normal_end - 4.0), (h.cx + 30.0, far_end + 4.0), TEAL_LIGHT, width=2.6, head=9.0)
    return c.finish()


FRAMES = {
    "retraction_length": 20,
    "retraction_speed": 20,
    "deretraction_speed": 20,
    "retract_restart_extra": 20,
    "retraction_when_cut": 21,
}
