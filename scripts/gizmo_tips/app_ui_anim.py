"""The plate settings dialog's Spiral vase pictures as loops: the nozzle printing the last two turns of the
vase of app_ui.py. Off, it goes round one level layer, steps up at the seam and goes round the next; on,
it climbs all the way round in one helix, with no step and no seam.
"""

import math

import numpy as np

from .app_ui import SEAM_ANGLE, VASE_BOTTOM, VASE_H, VASE_LH, VASE_WALL, _layer_ridges, _nozzle, _vase_radius
from .render import GIF_SIZE, M_ACCENT, M_PART, Camera, Overlay, bed, render, union

OUT = "app_ui"  # resources/images/<OUT>

TURNS = 2
FRAMES_PER_TURN = 8
HOLD = 4  # frames the finished vase is held before the loop starts over
# What one turn of the nozzle adds, in both modes: the layer height of the still, so that the two loops
# climb at the same rate and differ only in how.
RISE = VASE_LH
START = VASE_H - TURNS * RISE


def _vase(k, spiral):
    u = min(k / FRAMES_PER_TURN, TURNS)  # turns printed so far in this loop
    layer, frac = (TURNS - 1, 1.0) if u >= TURNS else (int(u), u - int(u))

    def scene(p):
        z = p[:, 2]
        r = np.hypot(p[:, 0], p[:, 1])
        a = (np.arctan2(p[:, 1], p[:, 0]) - SEAM_ANGLE) % (2.0 * math.pi) / (2.0 * math.pi)  # 0..1 round from the seam
        rz = _vase_radius(z)
        wall = np.abs(r - rz + VASE_WALL * 0.5) - VASE_WALL * 0.5
        if spiral:
            # The last pass of the nozzle over this angle, in turns: the rim is a helix ending at the nozzle.
            top = START + (u - (u - a) % 1.0) * RISE
            current = z > top - RISE
            ridge = _layer_ridges(z - a * RISE, RISE, 0.012)  # ridges that follow the helix, not level rings
        else:
            level = START + layer * RISE
            top = np.where(a < frac, level + RISE, level)
            current = z > level
            ridge = _layer_ridges(z, RISE, 0.012)
        shell = np.minimum(np.maximum(wall, np.maximum(-z, z - top)),
                           np.maximum(r - rz, np.maximum(-z, z - VASE_BOTTOM))) * 0.8
        mat = np.where(current & (z > VASE_BOTTOM + 0.01), M_ACCENT, M_PART)
        items = [bed(p), (shell + ridge * (z > VASE_BOTTOM), mat)]
        if not spiral:
            # The seam: where each layer starts and ends, a small blob in a line up the side.
            done = START + layer * RISE + (RISE if frac > 0.0 else 0.0)
            zi = np.clip(np.round(z / RISE), 1, round(done / RISE)) * RISE
            rs = _vase_radius(zi)
            c = np.stack([rs * math.cos(SEAM_ANGLE), rs * math.sin(SEAM_ANGLE), zi], axis=1)
            items.append((np.linalg.norm(p - c, axis=1) - 0.021, M_ACCENT))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.62), extent=2.0)
    o = Overlay(render(scene, cam, step=0.7), cam)
    if u < TURNS:
        turn = u if spiral else frac
        z = START + (u if spiral else layer + 1) * RISE
        ang = SEAM_ANGLE + 2.0 * math.pi * turn
        rz = float(_vase_radius(z)) - 0.5 * VASE_WALL
        _nozzle(o, (rz * math.cos(ang), rz * math.sin(ang), z), size=0.9)
    return o.finish(GIF_SIZE)


def spiral_vase_off(k, n):
    return _vase(k, False)


def spiral_vase_on(k, n):
    return _vase(k, True)


# name -> frame count
FRAMES = {
    "spiral_vase_off": TURNS * FRAMES_PER_TURN + HOLD,
    "spiral_vase_on": TURNS * FRAMES_PER_TURN + HOLD,
}
