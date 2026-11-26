"""Animated pictures for the filament settings whose point is a movement (SettingTipsFilament.cpp), in the
hotend section and palette of the tool change animations in settings_filament.py, into the same folder.

- The long retraction at an extruder change: the filament pulled from the nozzle up out of the hot zone,
  before its extruder is parked (long_retractions_when_ec, retraction_distances_when_ec).
"""

from .render import GIF_SIZE, phase, smooth
from .settings_filament import C_ACCENT_LIGHT, C_PART, Flat, _NOZZLE_TIP, _TUBE_TOP, _filament, _hotend

OUT = "settings/filament"  # resources/images/<OUT>


def ec_long_retraction(k, n):
    """In the nozzle, then drawn up to the top of the heat sink, and held there."""
    f = Flat(GIF_SIZE)
    _hotend(f)
    t = smooth(phase(k, 3, 9))  # frames 3..11
    top = _TUBE_TOP + 0.06
    tip = _NOZZLE_TIP + (top - _NOZZLE_TIP) * t
    _filament(f, tip, tip, "flat" if t == 0.0 else "point", melt=t == 0.0)
    base = _NOZZLE_TIP - 0.02
    f.line([(0.74, base), (0.86, base)], C_PART, 1.6)
    if base - tip > 0.06:
        f.arrow((0.80, base), (0.80, tip + 0.01), C_ACCENT_LIGHT, 2.6, 0.035)
    return f.finish()


FRAMES = {
    "ec_long_retraction": 20,
}
