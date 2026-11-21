"""The Mesh Boolean panel's tooltips (GLGizmoMeshBoolean::on_render_input_window()).

The three operations are shown with the toolbar's own pictures (gizmo_toolbar/boolean_*.png); this set
draws only what the toolbar has no picture for. Same cube and ball as those, so they read as one series.
"""

import numpy as np

from .render import M_ACCENT, M_PART, M_PART2, SIZE, Camera, Overlay, bed, render, sd_box, sd_sphere, union

OUT = "gizmos/boolean"  # resources/images/<OUT>


def _cube(p):
    return sd_box(p, (0, 0, 0.5), (0.5, 0.5, 0.5), 0.02)


def _ball(p):
    return sd_sphere(p, (0.45, -0.45, 0.95), 0.55)


def _difference(p):
    a, b = _cube(p), _ball(p)
    return np.maximum(a, -b), np.where(-b > a, M_ACCENT, M_PART)


def _camera():
    return Camera(SIZE, target=(0.1, -0.1, 0.55), extent=2.5)


def _intersection(p):
    a, b = _cube(p), _ball(p)
    return np.maximum(a, b), np.where(b > a, M_ACCENT, M_PART)


# Delete input: the result alone, or the result with Part 2 still there. Part 2 is drawn see-through,
# since solid it would hide the result inside it and look like the parts before the operation.
def _kept(result_fn):
    cam = _camera()
    result = render(lambda p: union(bed(p), result_fn(p)), cam)
    kept = render(lambda p: union(bed(p), result_fn(p), (_ball(p), M_PART2)), cam)
    return Overlay(result + (kept - result) * 0.5, cam).finish(SIZE)


def _deleted(result_fn):
    cam = _camera()
    return Overlay(render(lambda p: union(bed(p), result_fn(p)), cam), cam).finish(SIZE)


def difference_keep(k, n):
    return _kept(_difference)


def difference_delete(k, n):
    return _deleted(_difference)


def intersection_keep(k, n):
    return _kept(_intersection)


def intersection_delete(k, n):
    return _deleted(_intersection)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "difference_keep": 0,
    "difference_delete": 0,
    "intersection_keep": 0,
    "intersection_delete": 0,
}
