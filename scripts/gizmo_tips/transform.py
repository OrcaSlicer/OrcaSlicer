"""The Move, Rotate and Scale panels' tooltips (see GizmoObjectManipulation's panel_tooltip() calls).

Each comparison is two animations of the same model doing the same thing under the two values of a
setting, so whatever differs between them is the setting. The model is grey, the arrows are in the
colours of the axes they follow, as the handles in the view are, and ACCENT marks the line a move
goes along or to.
"""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmos/transform"  # resources/images/<OUT>

ACCENT_RGB = tuple(int(round(c * 255.0)) for c in ACCENT)
GUIDE = ACCENT_RGB + (235,)
# How far the model in the coordinate system pictures is turned about Z: away from the view's own axes
# as well as the plate's, so its edges line up with neither.
TURN = math.radians(-30.0)


def _turned_box(c, half, R, r=0.02):
    return lambda p: sd_box((p - c) @ R, (0.0, 0.0, 0.0), half, r)


def painted(floor, a, b, half_width=0.018):
    """`floor` with a line painted on it from a to b (x, y): the guide a move goes along or to. Painted
    rather than drawn over the picture, so a model standing on it hides it."""
    def f(p):
        d, mat = floor(p)
        on = sd_segment2(p[:, 0], p[:, 1], a, b) < half_width
        return d, np.where(on & (p[:, 2] < 0.01), M_ACCENT, mat)
    return f


def _hold_move_hold(k, start, length):
    """0 for the first frames, then a smooth step to 1 over `length` frames, then 1."""
    return smooth(phase(k, start, length))


# --- Move: the coordinate system -----------------------------------------------------------------


def _move(k, n, own_axes):
    R = rot_z(TURN)
    along = R @ np.array([1.0, 0.0, 0.0]) if own_axes else np.array([1.0, 0.0, 0.0])
    side = R @ np.array([0.0, 1.0, 0.0]) if own_axes else np.array([0.0, 1.0, 0.0])
    half = np.array([0.55, 0.28, 0.24])
    c = np.array([0.0, 0.0, half[2]]) + (-0.42 + 0.84 * ease(k / n)) * along
    body = _turned_box(c, half, R)
    floor = painted(bed, tuple(-1.4 * along[:2]), tuple(1.4 * along[:2]))  # the line the model slides along
    scene = lambda p: union(floor(p), (body(p), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=3.0, el=48.0)
    o = Overlay(render(scene, cam), cam)
    o.arrow(c, c + (0.0, 0.0, 0.75), AXIS_Z, width=2.4)
    o.arrow(c, c + 0.75 * side, AXIS_Y, width=2.4)
    o.arrow(c, c + 1.0 * along, AXIS_X, width=3.0, head=11.0)
    return o.finish(GIF_SIZE)


def move_world(k, n):
    return _move(k, n, own_axes=False)


def move_object(k, n):
    return _move(k, n, own_axes=True)


# --- Move: drop to the bed -----------------------------------------------------------------------


def drop(k, n):
    R = rot_z(math.radians(25.0)) @ rot_y(math.radians(28.0)) @ rot_x(math.radians(20.0))
    half = np.array([0.5, 0.32, 0.26])
    verts = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half
    world = verts @ R.T
    low = world[world[:, 2].argmin()]  # the corner that ends up on the bed, relative to the centre
    lift = 0.6 * (1.0 - _hold_move_hold(k, 5, 6))
    c = np.array([0.0, 0.0, -low[2] + lift])
    body = _turned_box(c, half, R)
    scene = lambda p: union(bed(p), (body(p), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.55), extent=2.6)
    o = Overlay(render(scene, cam), cam)
    tip = c + low
    foot = np.array([tip[0], tip[1], 0.0])
    if k < 5:
        o.arrow(tip, foot + (0.0, 0.0, 0.03), GUIDE, width=2.2, head=8.0)
        o.circle_on(foot, (0.0, 0.0, 1.0), 0.07, GUIDE, width=1.4)
    elif lift < 1e-6:
        o.circle_on(foot, (0.0, 0.0, 1.0), 0.07, GUIDE, width=1.4, fill=GUIDE)
    return o.finish(GIF_SIZE)


# --- Move: align within the selection or to the plate ----------------------------------------------

PLATE = 1.3  # half the side of the plate drawn in the align pictures
# Three items of different sizes, scattered: (centre x, centre y), half size.
ITEMS = [((-0.15, 0.68), (0.32, 0.24, 0.30)), ((0.55, 0.02), (0.22, 0.22, 0.20)), ((0.12, -0.70), (0.26, 0.20, 0.40))]


def plate(p):
    return sd_box(p, (0.0, 0.0, -0.05), (PLATE, PLATE, 0.05)), M_BED


def _align(k, n, to_plate):
    lefts = [x - h[0] for (x, _), h in ITEMS]
    line = -PLATE if to_plate else min(lefts)
    t = _hold_move_hold(k, 4, 8)
    if to_plate:  # the group moves as one, keeping its spacing
        shifts = [line - min(lefts)] * len(ITEMS)
    else:  # each item moves to the line on its own
        shifts = [line - left for left in lefts]
    boxes = [(np.array([x + t * s, y, h[2]]), h) for ((x, y), h), s in zip(ITEMS, shifts)]

    x = line + 0.02 if to_plate else line  # kept on the plate
    floor = painted(plate, (x, -PLATE), (x, PLATE), 0.02)

    def scene(p):
        return union(floor(p), *[(sd_box(p, c, h, 0.02), M_PART) for c, h in boxes])

    cam = Camera(GIF_SIZE, target=(-0.15, 0.0, 0.15), extent=3.3, el=42.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def align_selection(k, n):
    return _align(k, n, to_plate=False)


def align_plate(k, n):
    return _align(k, n, to_plate=True)


# --- Scale: the coordinate system ----------------------------------------------------------------


def _scale(k, n, own_axes):
    s = 1.0 + 0.6 * ease(k / n)
    R = rot_z(TURN)
    half = np.array([0.5, 0.3, 0.3])
    c = np.array([0.0, 0.0, half[2]])
    if own_axes:
        body = lambda p: sd_box((p - c) @ R, (0.0, 0.0, 0.0), half * (s, 1.0, 1.0), 0.02)
        along = R @ np.array([1.0, 0.0, 0.0])
    else:
        # Stretched along the plate's X after being turned: a shear in the model's own frame. The distance
        # in that frame never overestimates the true one, since the stretch only makes things larger.
        S_inv = np.diag([1.0 / s, 1.0, 1.0])
        body = lambda p: sd_box((p - c) @ S_inv @ R, (0.0, 0.0, 0.0), half, 0.02)
        along = np.array([1.0, 0.0, 0.0])
    scene = lambda p: union(bed(p), (body(p), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.25), extent=2.9, el=50.0)
    o = Overlay(render(scene, cam), cam)
    reach = 0.55 * s + 0.35
    top = c + (0.0, 0.0, half[2])
    o.arrow(top, top + reach * along, AXIS_X, width=3.0, head=11.0)
    o.arrow(top, top - reach * along, AXIS_X, width=3.0, head=11.0)
    return o.finish(GIF_SIZE)


def scale_world(k, n):
    return _scale(k, n, own_axes=False)


def scale_object(k, n):
    return _scale(k, n, own_axes=True)


# --- Scale: uniform scale on and off -------------------------------------------------------------


def _uniform(k, n, on):
    t = ease(k / n)
    f = np.array([1.0 + 0.45 * t] * 3) if on else np.array([1.0 + 0.7 * t, 1.0, 1.0])
    r, h = 0.4, 0.6
    # A round model, so a stretch along one axis shows as an oval. Scaling the points by 1/f underestimates
    # the distance by at most the smallest factor, which is 1.
    body = lambda p: sd_cylinder(p / f, (0.0, 0.0), r, 0.0, h, 0.03)
    scene = lambda p: union(bed(p), (body(p), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.4), extent=2.7, el=34.0)
    o = Overlay(render(scene, cam), cam)
    top = np.array([0.0, 0.0, h * f[2]])
    o.arrow(top, top + (r * f[0] + 0.4, 0.0, 0.0), AXIS_X, width=2.8, head=10.0)
    if on:
        o.arrow(top, top + (0.0, r * f[1] + 0.4, 0.0), AXIS_Y, width=2.8, head=10.0)
        o.arrow(top, top + (0.0, 0.0, 0.25 + 0.35 * t), AXIS_Z, width=2.8, head=10.0)
    return o.finish(GIF_SIZE)


def uniform_on(k, n):
    return _uniform(k, n, on=True)


def uniform_off(k, n):
    return _uniform(k, n, on=False)


FRAMES = {
    "move_world": 16,
    "move_object": 16,
    "drop": 18,
    "align_selection": 20,
    "align_plate": 20,
    "scale_world": 16,
    "scale_object": 16,
    "uniform_on": 16,
    "uniform_off": 16,
}
