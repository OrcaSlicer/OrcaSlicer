"""The gizmo panels' pictures whose point is a movement, drawn as loops: a brush stroke being made, a part
turned over after a cut or by an assembly flip, the two picks of an assembly. They replace stills of the
cut, measure and painting sets and land in those sets' folders, so each name here carries its folder
(gen_gizmo_tips.py finds the function through the module's __getattr__ below).

Each loop runs the action once and holds the result before it starts over. The scenes are the stills'
own, from the modules they were drawn in, at the animations' size.
"""

import math

import numpy as np

from . import cut, measure, painting
from .render import (CURSOR, GIF_SIZE, M_ACCENT, M_PART, M_PART2, Camera, Overlay, bed, phase, render,
                     rot_axis, sd_box, sd_cylinder, smooth, union, SS)

OUT = "gizmos"  # resources/images/<OUT>; each picture's name starts with its gizmo's folder

FRAMES_PER_LOOP = 20


# --- painting: Vertical and Horizontal strokes -----------------------------------------------------
#
# The wall of the stills. The mouse wanders up (or across) the wall, leaving the dashed trail; with the
# option off the paint follows it, with it on the paint runs straight from where the stroke started.

_STROKE_FRAMES = 15  # drawing; the rest of the loop holds the finished stroke


def _stroke(k, direction, locked):
    y = -painting._WALL[1] - 0.001
    t = np.linspace(0.0, 1.0, 40)
    wobble = 0.09 * np.sin(t * 2.0 * math.pi * 1.6) + 0.04 * np.sin(t * 2.0 * math.pi * 3.7)
    if direction == "vertical":
        mouse = [np.array([-0.15 + w, y, 0.18 + 0.85 * s]) for s, w in zip(t, wobble)]
        stroke = [np.array([mouse[0][0], y, m[2]]) for m in mouse] if locked else mouse
    else:
        mouse = [np.array([-0.55 + 1.1 * s, y, 0.55 + w]) for s, w in zip(t, wobble)]
        stroke = [np.array([m[0], y, mouse[0][2]]) for m in mouse] if locked else mouse
    upto = 2 + int(round((len(mouse) - 2) * phase(k, 0, _STROKE_FRAMES)))
    mouse, stroke = mouse[:upto], stroke[:upto]
    stroke_r = 0.1

    def painted(q):
        front = q[:, 1] < -painting._WALL[1] + 0.01
        return np.where(front & (painting._path_dist(q, stroke) < stroke_r), M_ACCENT, M_PART)

    def scene(p):
        d = sd_box(p, (0.0, 0.0, painting._WALL[2]), painting._WALL, 0.02)
        return union(bed(p), (d, painting._shade(d, p, painted)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.6), extent=2.2, az=28.0, el=22.0)
    o = Overlay(render(scene, cam), cam)
    painting._dashed(o, mouse, painting.PATH)
    o.circle_on(stroke[-1], (0.0, -1.0, 0.0), stroke_r, CURSOR)
    o.pointer(mouse[-1])
    return o.finish(GIF_SIZE)


# --- cut: Place on cut and Flip ---------------------------------------------------------------------
#
# The rocket's part as the cut leaves it, then turned into the pose the option gives it (the stills'
# final poses, cut._part_pose), lifted a little while it turns so that it clears the bed.

def _axis_angle(R):
    angle = math.acos(np.clip((np.trace(R) - 1.0) * 0.5, -1.0, 1.0))
    if angle < 1e-9:
        return np.array([0.0, 0.0, 1.0]), 0.0
    if math.pi - angle < 1e-6:  # a half turn: the axis is the column of (R + I) / 2 that is not zero
        m = 0.5 * (R + np.eye(3))
        axis = m[:, int(np.argmax(np.diag(m)))]
    else:
        axis = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]])
    return axis / np.linalg.norm(axis), angle


def _part_turn(k, upper, how):
    axis, angle = _axis_angle(cut._part_pose(upper, how))
    s = smooth(phase(k, 4, 10))
    R = rot_axis(axis, angle * s)
    pts, rim = cut._rocket_points()
    side = (pts - cut.CUT_P) @ cut.CUT_N
    keep = np.concatenate([pts[side >= -1e-9] if upper else pts[side <= 1e-9], rim])
    placed = keep @ R.T
    centre = 0.5 * (placed.min(axis=0) + placed.max(axis=0))
    offset = np.array([-centre[0], -centre[1], -placed[:, 2].min() + 0.3 * math.sin(math.pi * s)])

    def scene(p):
        q = (p - offset) @ R
        plane = (q - cut.CUT_P) @ cut.CUT_N
        return union(bed(p), cut._cut_mat(cut._rocket(q), -plane if upper else plane))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=1.8)
    return Overlay(render(scene, cam, step=0.8), cam).finish(GIF_SIZE)


# --- measure: Flip by Face 2 ------------------------------------------------------------------------
#
# The part on the table of the stills turns over about the centre of its wide face, Face 2, which stays
# on the table top and ends up facing the other way. It turns about the horizontal line of sight, so the
# movement lies in the picture.

def _flip_turn(k):
    top = measure._TABLE_TOP
    pivot = np.array([0.0, -0.05, top + 0.003])  # ends a hair above the table top rather than in it
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.7), extent=2.5, el=9.0)
    R = rot_axis((cam.c[0], cam.c[1], 0.0), math.pi * smooth(phase(k, 4, 10)))

    def scene(p):
        slab = sd_box(p, (0.0, 0.0, top - 0.06), (0.9, 0.62, 0.06), 0.012)
        legs = [sd_cylinder(p, (sx * 0.78, sy * 0.5), 0.06, 0.0, top - 0.1) for sx in (-1, 1) for sy in (-1, 1)]
        q = (p - pivot) @ R + pivot
        part = measure._frustum(q, (0.0, -0.05), top, 0.36, top + 0.55, 0.14)
        return union(bed(p), (np.minimum(slab, np.minimum.reduce(legs)), M_PART), (part, M_ACCENT))

    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


# --- measure: the assembly modes, picked one after the other ----------------------------------------
#
# The pointer goes to the fixed part and clicks, its feature lights up; then to the moving part, the
# same; then what is measured between the two appears.

_CLICK_1, _CLICK_2, _RESULT = 5, 11, 13


def _pointer_at(o, k, first, second):
    """The pointer, gliding in from below left to the first pick and on to the second (scene points)."""
    b, c = (np.array(o.pt(x)) for x in (first, second))
    a = b + np.array([-34.0, 30.0]) * SS
    if k < _CLICK_1:
        xy = a + (b - a) * smooth(phase(k, 0, _CLICK_1))
    else:
        xy = b + (c - b) * smooth(phase(k, _CLICK_1 + 1, _CLICK_2 - _CLICK_1 - 1))
    o.pointer(xy=tuple(xy))


def _mode_face(k):
    cam = measure._assembly_camera(GIF_SIZE)
    face = measure._face2(cam)
    along = np.abs(face) @ measure._PART_H
    top = 2.0 * measure._BASE_H[2]
    R, c = measure._R0, measure._C0
    pick1, pick2 = k >= _CLICK_1, k >= _CLICK_2

    def scene(p):
        a = sd_box(p, measure._BASE_C, measure._BASE_H, 0.015)
        q = (p - c) @ R
        b = sd_box(q, (0, 0, 0), measure._PART_H, 0.015)
        return union(bed(p), (a, np.where(pick1 & (p[:, 2] > top - 0.01), M_ACCENT, M_PART)),
                     (b, np.where(pick2 & (q @ face > along - 0.01), M_ACCENT, M_PART2)))

    o = Overlay(render(scene, cam), cam)
    f1 = np.array([measure._BASE_C[0], measure._BASE_C[1], top])
    f2 = c + R @ (face * along)
    if k >= _RESULT:
        measure._dashed(o, f2, f1, measure.WHITE, width=1.2)
    if pick1:
        o.dot(f1, 3.0, measure.WHITE, outline=measure.DOT_OUTLINE)
    if pick2:
        o.dot(f2, 3.0, measure.WHITE, outline=measure.DOT_OUTLINE)
    _pointer_at(o, k, f1 + (0.06, -0.08, 0.0), f2)
    return o.finish(GIF_SIZE)


def _mode_point(k):
    cam = measure._assembly_camera(GIF_SIZE)
    o = Overlay(render(lambda p: measure._mode_scene(p, True), cam), cam)
    hole = np.array([-0.45, -0.1, 0.5])
    boss = np.array([0.62, 0.25, 1.05])
    if k >= _CLICK_1:
        o.circle_on(hole, (0, 0, 1), 0.2, measure.LINE, width=2.6)
    if k >= _CLICK_2:
        o.circle_on(boss, (0, 0, 1), 0.2, measure.LINE, width=2.6)
    if k >= _RESULT:
        measure._xyz_legs(o, hole, boss, width=2.2)
    if k >= _CLICK_1:
        measure._picked(o, hole, 3.0)
    if k >= _CLICK_2:
        measure._picked(o, boss, 3.0)
    _pointer_at(o, k, hole + (0.2, 0.0, 0.0), boss + (0.2, 0.0, 0.0))
    return o.finish(GIF_SIZE)


# --- the list ------------------------------------------------------------------------------------

_PICTURES = {
    "painting/stroke_vertical_off": lambda k, n: _stroke(k, "vertical", False),
    "painting/stroke_vertical_on": lambda k, n: _stroke(k, "vertical", True),
    "painting/stroke_horizontal_off": lambda k, n: _stroke(k, "horizontal", False),
    "painting/stroke_horizontal_on": lambda k, n: _stroke(k, "horizontal", True),
    "cut/upper_on_cut": lambda k, n: _part_turn(k, True, "on_cut"),
    "cut/upper_flip": lambda k, n: _part_turn(k, True, "flip"),
    "cut/lower_on_cut": lambda k, n: _part_turn(k, False, "on_cut"),
    "cut/lower_flip": lambda k, n: _part_turn(k, False, "flip"),
    "measure/flip_on": lambda k, n: _flip_turn(k),
    "measure/mode_face": lambda k, n: _mode_face(k),
    "measure/mode_point": lambda k, n: _mode_point(k),
}


def __getattr__(name):
    """The generator looks a picture up by its name, which here holds a slash."""
    try:
        return _PICTURES[name]
    except KeyError:
        raise AttributeError(name) from None


# name -> frame count
FRAMES = {name: FRAMES_PER_LOOP for name in _PICTURES}
