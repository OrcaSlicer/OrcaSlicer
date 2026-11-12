"""The send-print dialog's tooltips: what bed leveling and the two timelapse modes do on the printer
(see PrintOption::set_about() in SelectMachine.cpp and the printing options of SendMultiMachinePage)."""

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "device"  # resources/images/<OUT>

# Toolhead colour: the darker grey, so the head stays apart from the grey model it prints.
M_HEAD = M_PART2


def _head(p, tip):
    """The toolhead with its nozzle tip at `tip`: a block, a heater, a short nozzle."""
    tip = np.asarray(tip, float)
    nozzle = sd_cylinder(p, tip[:2], 0.04, tip[2], tip[2] + 0.16, 0.01)
    heater = sd_cylinder(p, tip[:2], 0.09, tip[2] + 0.13, tip[2] + 0.24, 0.02)
    block = sd_box(p, tip + (0.0, 0.0, 0.42), (0.2, 0.17, 0.18), 0.03)
    return np.minimum(np.minimum(nozzle, heater), block)


# --- bed leveling --------------------------------------------------------------------------------
#
# The nozzle touches the bed at a grid of points, each one marked as it is measured. Two frames a
# point (above it, touching it), then a hold on the finished grid with the head parked.

_PROBES = [(x, y) for j, y in enumerate((-0.7, 0.0, 0.7)) for x in ((-0.7, 0.0, 0.7) if j % 2 == 0 else (0.7, 0.0, -0.7))]
_PROBE_FRAMES = 2 * len(_PROBES)


def bed_leveling(k, n):
    if k < _PROBE_FRAMES:
        i, down = divmod(k, 2)
        x, y = _PROBES[i]
        tip = (x, y, 0.0 if down else 0.3)
        probed = i + (1 if down else 0)
    else:
        tip = (0.0, 1.15, 0.5)
        probed = len(_PROBES)
    marks = np.array(_PROBES)

    def scene(p):
        d_bed, _ = bed(p)
        dist = np.hypot(p[:, None, 0] - marks[None, :, 0], p[:, None, 1] - marks[None, :, 1])
        near = dist.argmin(axis=1)
        on_mark = dist.min(axis=1) < 0.11
        mat = np.where(on_mark, np.where(near < probed, M_ACCENT, M_PART2), M_BED)
        return union((d_bed, mat), (_head(p, tip), M_HEAD))

    cam = Camera(GIF_SIZE, target=(0.0, 0.1, 0.35), extent=2.9)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


# --- timelapse -----------------------------------------------------------------------------------
#
# A part grows layer by layer, and a frame of the video is taken at the end of each layer, shown by the
# camera's corner marks lighting up. In the traditional mode the head is wherever the layer ended, so
# the video shows it all over the part; in the smooth mode it moves aside to the prime tower first, so
# every frame shows the part alone.

_LAYERS = 5
_PER_LAYER = 4
_LAYER_H = 0.16
_PART = (np.array([-0.2, -0.1]), np.array([0.5, 0.4]))  # centre and half size in x, y
_TOWER = np.array([0.95, 0.75])


def _corners(o, size, colour):
    """The camera's frame marks at the corners of the picture."""
    m, L, w = 14 * SS, 26 * SS, max(1, int(round(3.0 * SS)))
    s = size * SS
    for cx, cy, dx, dy in ((m, m, 1, 1), (s - m, m, -1, 1), (m, s - m, 1, -1), (s - m, s - m, -1, -1)):
        o.draw.line([(cx, cy + dy * L), (cx, cy), (cx + dx * L, cy)], fill=colour, width=w, joint="curve")


def _timelapse(k, smooth_mode):
    layer, step = divmod(k, _PER_LAYER)
    top = _LAYER_H * (layer + 1)
    centre, half = _PART
    # The head runs along the layer for the first frames of it.
    xs = (-0.55, 0.05, 0.3)
    if step < 2 or not smooth_mode:
        x = xs[min(step, 2) if not smooth_mode else step]
        y = centre[1] + (0.25 if layer % 2 else -0.25)
        tip = (x, y, top)
    else:
        tip = (_TOWER[0], _TOWER[1], top)
    snapshot = step == _PER_LAYER - 1

    def scene(p):
        items = [bed(p), (sd_box(p, (centre[0], centre[1], 0.5 * top), (half[0], half[1], 0.5 * top), 0.02), M_PART),
                 (_head(p, tip), M_HEAD)]
        if smooth_mode:
            items.append((sd_box(p, (_TOWER[0], _TOWER[1], 0.5 * top), (0.16, 0.16, 0.5 * top), 0.01), M_PART))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.25, 0.25, 0.45), extent=3.0)
    o = Overlay(render(scene, cam), cam)
    _corners(o, GIF_SIZE, (60, 200, 180) if snapshot else (110, 116, 118))
    return o.finish(GIF_SIZE)


def timelapse_traditional(k, n):
    return _timelapse(k, False)


def timelapse_smooth(k, n):
    return _timelapse(k, True)


FRAMES = {
    "bed_leveling": _PROBE_FRAMES + 3,
    "timelapse_traditional": _LAYERS * _PER_LAYER,
    "timelapse_smooth": _LAYERS * _PER_LAYER,
}
