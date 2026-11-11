"""More of the Device tab's tooltips: skipping objects during a print (PartSkipDialog.cpp) and the two
ways of synchronizing the project's filaments from the AMS (SyncAmsInfoDialog.cpp)."""

import numpy as np
from PIL import Image, ImageDraw

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "device"  # resources/images/<OUT>


# --- skipping objects ----------------------------------------------------------------------------
#
# Four objects grow layer by layer. One of them is picked and turns the accent colour, and from then on
# it stays at the height it had while the other three are printed to the end.

_OBJECTS = [  # (kind, centre x, centre y)
    ("box", -0.55, -0.45),
    ("cyl", 0.55, -0.45),
    ("cyl", -0.55, 0.55),
    ("box", 0.55, 0.55),
]
_SKIPPED = 1
_GROW, _PICK, _FINISH, _HOLD = 6, 3, 8, 4
_LOW, _FULL = 0.32, 0.85


def _object(p, kind, x, y, h):
    if kind == "box":
        return sd_box(p, (x, y, 0.5 * h), (0.3, 0.3, 0.5 * h), 0.02)
    return sd_cylinder(p, (x, y), 0.3, 0.0, h, 0.02)


def skip_objects(k, n):
    if k < _GROW:
        rest = skipped = _LOW * (k + 1) / _GROW
        picked = False
    elif k < _GROW + _PICK:
        rest = skipped = _LOW
        picked = k > _GROW
    else:
        t = min(1.0, (k - _GROW - _PICK + 1) / _FINISH)
        rest, skipped, picked = _LOW + (_FULL - _LOW) * t, _LOW, True

    def scene(p):
        items = [bed(p)]
        for i, (kind, x, y) in enumerate(_OBJECTS):
            mine = i == _SKIPPED
            items.append((_object(p, kind, x, y, skipped if mine else rest), M_ACCENT if mine and picked else M_PART))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, 0.05, 0.35), extent=2.9)
    o = Overlay(render(scene, cam), cam)
    if _GROW <= k < _GROW + _PICK:
        _, x, y = _OBJECTS[_SKIPPED]
        o.pointer((x - 0.05, y - 0.1, _LOW))
    return o.finish(GIF_SIZE)


# --- synchronizing the filaments from the AMS ----------------------------------------------------
#
# Seen as a diagram: the project's filament list along the top, the AMS with its four slots along the
# bottom. Mapping joins each project filament to the slot it is mapped to, in any order, and the
# filament takes that slot's colour; the list keeps its length. Overwriting joins them in order, slot
# to filament, and the slot with no filament above it adds one to the end of the list.

_BG = tuple(int(round(c * 0.75 * 255)) for c in BED)
_FRAME = (110, 116, 118)
_LINK = tuple(int(c * 255) for c in ACCENT)
# Muted filament colours, so that they read as colours without drawing the eye from the links.
_PROJECT = [(196, 92, 84), (222, 196, 104), (226, 228, 228)]
_SLOTS = [(84, 128, 200), (120, 170, 96), (150, 112, 182), (232, 152, 72)]
_MAP = [2, 0, 3]  # project filament -> slot, for Mapping

_S = 2  # drawn at twice the size and shrunk, for smooth edges


def _layout(size):
    """The centres of the four positions of a row, and the y of the two rows."""
    w = size * _S
    xs = [w * (0.2 + 0.2 * i) for i in range(4)]
    return xs, 0.27 * w, 0.73 * w


def _swatch(d, x, y, half, colour, outline=(30, 32, 34)):
    d.rounded_rectangle([x - half, y - half, x + half, y + half], radius=half * 0.3, fill=colour, outline=outline,
                        width=_S * 2)


def _sync(k, overwrite):
    size = GIF_SIZE
    img = Image.new("RGB", (size * _S, size * _S), _BG)
    d = ImageDraw.Draw(img)
    xs, y_top, y_slot = _layout(size)
    half = 0.085 * size * _S

    # The AMS: a frame round its four slots.
    d.rounded_rectangle([xs[0] - 1.7 * half, y_slot - 1.9 * half, xs[3] + 1.7 * half, y_slot + 1.9 * half],
                        radius=half * 0.5, outline=_FRAME, width=_S * 2)
    for x, c in zip(xs, _SLOTS):
        d.rounded_rectangle([x - 0.8 * half, y_slot - 1.4 * half, x + 0.8 * half, y_slot + 1.4 * half],
                            radius=half * 0.3, fill=c, outline=(30, 32, 34), width=_S * 2)

    # Hold, then one link a step, then the colours change, then hold the result.
    links = 4 if overwrite else 3
    shown = max(0, min(links, (k - 3) // 2 + 1)) if k >= 3 else 0
    changed = k >= 3 + 2 * links + 1
    pairs = [(i, i) for i in range(4)] if overwrite else list(enumerate(_MAP))

    for i, s in pairs[:shown]:
        d.line([(xs[i], y_top + half), (xs[s], y_slot - 1.4 * half)], fill=_LINK, width=_S * 3)

    count = 4 if overwrite and changed else 3
    for i in range(count):
        if changed:
            colour = _SLOTS[i] if overwrite else _SLOTS[_MAP[i]]
        else:
            colour = _PROJECT[i]
        _swatch(d, xs[i], y_top, half, colour)
    if overwrite and not changed:
        # Where the list will grow: an empty place at its end.
        d.rounded_rectangle([xs[3] - half, y_top - half, xs[3] + half, y_top + half], radius=half * 0.3,
                            outline=_FRAME, width=_S)
    return np.asarray(img.resize((size, size), Image.LANCZOS))


def sync_mapping(k, n):
    return _sync(k, False)


def sync_overwrite(k, n):
    return _sync(k, True)


FRAMES = {
    "skip_objects": _GROW + _PICK + _FINISH + _HOLD,
    "sync_mapping": 3 + 2 * 3 + 1 + 6,
    "sync_overwrite": 3 + 2 * 4 + 1 + 6,
}
