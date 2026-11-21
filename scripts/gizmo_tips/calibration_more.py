"""The printer calibration dialog's tooltips (Calibration.cpp): what vibration compensation takes off a
print's walls."""

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "calibration"  # resources/images/<OUT>

# --- vibration compensation ----------------------------------------------------------------------
#
# A box printed fast. Without the compensation, the head shakes after each corner and leaves ripples on
# the walls that die away along them (ringing); with it, the walls are flat. The ripples stay grey: their
# shading shows them, and a colour on them read as a painted stripe rather than as a shape.

_HALF = np.array([0.6, 0.45, 0.4])
_WAVE = 0.11  # ripple wavelength
_DECAY = 0.32  # how far along the wall the ripples last
_DEPTH = 0.03


def _ripples(s):
    """The ripple at a distance `s` along a wall from the corner the head came round."""
    s = np.maximum(s, 0.0)
    return _DEPTH * np.exp(-s / _DECAY) * np.sin(2.0 * np.pi * s / _WAVE)


def _box(p, ringing):
    c = np.array([0.0, 0.0, _HALF[2]])
    d = sd_box(p, c, _HALF, 0.015)
    if ringing:
        # Front wall (y = -half): ripples run along x from the left corner. Right wall (x = +half): along y
        # from the front corner. Each fades out before the next wall, so the two do not meet at the corner.
        front = np.clip(1.0 - np.abs(p[:, 1] + _HALF[1]) / 0.08, 0.0, 1.0) * (p[:, 0] < _HALF[0] - 0.05)
        right = np.clip(1.0 - np.abs(p[:, 0] - _HALF[0]) / 0.08, 0.0, 1.0) * (p[:, 1] > -_HALF[1] + 0.05)
        bump = front * _ripples(p[:, 0] + _HALF[0]) + right * _ripples(p[:, 1] + _HALF[1])
        d = d - bump
    return d, M_PART


def _vibration(ringing):
    def scene(p):
        return union(bed(p), _box(p, ringing))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.35), extent=2.1)
    return Overlay(render(scene, cam, step=0.7, steps=220), cam).finish(SIZE)


def vibration_ringing(k, n):
    return _vibration(True)


def vibration_clean(k, n):
    return _vibration(False)


FRAMES = {
    "vibration_ringing": 0,
    "vibration_clean": 0,
}
