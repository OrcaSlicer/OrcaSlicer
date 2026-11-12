"""The feature pictures of the Process settings: feature_<name>.png, one per kind of line.

The settings that change one kind of line (its speed, acceleration, jerk, width, flow or filament) all
point at the picture of that line, on whichever page they are. Each picture shows one small part chosen
for its line, large in the frame, with that line in ACCENT and every other line grey, drawn in the
plater's view from the G-code OrcaSlicer sliced for it (settings_process_features_slice.py). Where the
line is inside the part, the part is cut open: a rectangle of it is left out above some height, so that
the layer at that height shows from above.
"""

import gzip
import json

import numpy as np

from .render import ACCENT, PART, PART2, SIZE, Camera
from .settings_process import Model, _clip, _partial, _paths
from .settings_process_features_slice import CX, DATA, TOWER

OUT = "settings/process"  # resources/images/<OUT>

_data = None


def _case(case):
    global _data
    if _data is None:
        with gzip.open(DATA, "rt") as f:
            _data = json.load(f)
    return _data[case]


LIT = ACCENT * 1.15
SCALE = 0.1  # scene units per mm
MARGIN = 1.12  # the frame around the part, as a share of its size on screen
TRAVEL_W = 0.18  # how wide a travel move is drawn, in mm
TRAVEL_MIN = 3.0  # mm: shorter travel moves, inside one part, are left out
LIFT = 0.25  # how far above its layer a travel move is drawn, so that the layer does not hide it


def _cut(paths, z, cuts):
    """The paths of the layer at height z, less what the cuts take away: (x0, x1, y0, y1, h) leaves out
    what lies in the rectangle above h."""
    for x0, x1, y0, y1, h in cuts:
        if z <= h + 1e-3:
            continue
        band = _clip(_clip(paths, (-1.0, 0.0), -x0), (1.0, 0.0), x1)
        paths = (_clip(paths, (1.0, 0.0), x0) + _clip(paths, (-1.0, 0.0), -x1)
                 + _clip(band, (0.0, 1.0), y0) + _clip(band, (0.0, -1.0), -y1))
    return paths


def _stray(p):
    """The travel in from the printer's home and back to it, and the extruding move off the prime tower
    that the G-code still labels as the tower: drawn nowhere."""
    pts = p["pts"]
    if p["t"] == "Travel":
        return np.abs(pts).max() > 30.0
    return p["t"] == "Prime tower" and pts[:, 0].max() > TOWER[0] - CX + 9.0


def _framed(items, az, el):
    """The view that fits the items: the plater's camera turned to (az, el), centred on them."""
    cam = Camera(SIZE, target=(0.0, 0.0, 0.0), az=az, el=el)
    pts = np.concatenate([np.column_stack([p["pts"], np.full(len(p["pts"]), z)]) for z, _, p, _ in items]) * SCALE
    u, v = pts @ cam.right, pts @ cam.up
    centre = cam.right * (u.max() + u.min()) / 2 + cam.up * (v.max() + v.min()) / 2
    # The camera's target is any point on the line through the centre along the view.
    return dict(target=tuple(centre), extent=max(u.max() - u.min(), v.max() - v.min()) * MARGIN, az=az, el=el)


def _picture(case, accent, cuts=(), az=35.0, el=32.0, travel=False, ironing=0.0):
    """One feature picture: the lines of `case` that pass accent(z, type) in ACCENT, the rest grey, the
    second filament's darker. travel: draw the moves between parts on the top layer. ironing: how much of
    the ironing to draw over the top surface, 0 for none."""
    def picture(k, n):
        layers = _case(case)
        top = max(layer["z"] for layer in layers)
        items = []
        for layer in layers:
            z = layer["z"]
            for p in _cut([p for p in _paths(layer, lambda t: True) if not _stray(p)], z, cuts):
                if p["t"] == "Travel":
                    if travel and z == top and np.linalg.norm(np.diff(p["pts"], axis=0), axis=1).sum() > TRAVEL_MIN:
                        items.append((z + LIFT, TRAVEL_W, dict(p, w=TRAVEL_W), LIT * 1.1))
                elif p["t"] == "Ironing":
                    continue
                else:
                    grey = PART2 * 0.75 if p.get("f") else PART
                    items.append((z, 0.25, p, LIT if accent(z, p["t"]) else grey))
            if ironing:
                for p in _partial(_paths(layer, lambda t: t == "Ironing"), ironing):
                    items.append((z + 0.08, 0.15, dict(p, w=0.22), LIT))
        return Model(items, SIZE, scale=SCALE, **_framed(items, az, el)).finish()
    return picture


def _is(*types):
    return lambda z, t: t in types


# The block is 14 mm square, 7.2 mm tall: three walls, sparse infill, three bottom layers and four top.
OPEN = (-2.0, 8.0, -8.0, 2.0)  # its front right corner, cut away to show the layers inside
PICTURES = {
    "outer_wall": _picture("block", _is("Outer wall"), [OPEN + (1.8,)]),
    "inner_wall": _picture("block", _is("Inner wall"), [OPEN + (1.8,)]),
    "top_surface": _picture("block", _is("Top surface")),
    "ironing": _picture("block", lambda z, t: False, ironing=0.6),
    "bottom_surface": _picture("block", _is("Bottom surface"), [OPEN + (0.3,)]),
    "internal_solid_infill": _picture("block", _is("Internal solid infill"), [OPEN + (0.9,)]),
    "sparse_infill": _picture("block", _is("Sparse infill"), [OPEN + (2.7,)]),
    # The bridge over the sparse infill, and in front of it, the sparse infill it lies on.
    "internal_bridge": _picture("block", _is("Internal Bridge"), [OPEN + (6.3,), (3.5, 8.0, -8.0, -3.5, 5.7)]),
    # The deck's first layer, with the back of the deck still on it.
    "bridge": _picture("bridge", _is("Bridge"), [(-12.0, 12.0, -8.0, 1.0, 5.4)]),
    "gap_fill": _picture("bars", _is("Gap infill"), el=55.0),
    "overhang": _picture("overhang", _is("Overhang wall"), az=15.0, el=10.0),
    # The front half of the slab cut away, down to the top of the supports or of their interface.
    "support": _picture("support", _is("Support", "Support transition"), [(-12.0, 12.0, -10.0, 0.5, 5.0)]),
    "support_interface": _picture("support", _is("Support interface"), [(-12.0, 12.0, -10.0, 0.5, 5.85)]),
    "skirt": _picture("bed", _is("Skirt")),
    "brim": _picture("bed", _is("Brim")),
    "first_layer": _picture("bed", lambda z, t: z < 0.35, [(-1.0, 15.0, -15.0, 1.0, 0.3)]),
    "travel": _picture("travel", lambda z, t: False, travel=True),
    "prime_tower": _picture("prime_tower", _is("Prime tower")),
}

FRAMES = {}
for _name, _fn in PICTURES.items():
    FRAMES["feature_" + _name] = 0
    globals()["feature_" + _name] = _fn
