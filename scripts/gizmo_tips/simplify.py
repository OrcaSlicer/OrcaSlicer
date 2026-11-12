"""The Simplify panel's tooltips (GLGizmoSimplify::on_render_input_window())."""

import numpy as np

from .render import M_PART, SIZE, Camera, Overlay, bed, render, union
from .toolbar import _fibonacci

OUT = "gizmos/simplify"  # resources/images/<OUT>


def _ball(facets):
    """The toolbar's Simplify ball, cut from `facets` planes: fewer of them is a coarser mesh."""
    normals = _fibonacci(facets)
    c = np.array([0.0, 0.0, 0.62])

    def scene(p):
        return union(bed(p), (((p - c) @ normals.T).max(axis=1) - 0.6, M_PART))

    cam = Camera(SIZE, target=(0.0, 0.0, 0.62), extent=2.0)
    return Overlay(render(scene, cam), cam).finish(SIZE)


# The Detail level slider, at three of its five steps.
def detail_extra_high(k, n):
    return _ball(320)


def detail_medium(k, n):
    return _ball(70)


def detail_extra_low(k, n):
    return _ball(18)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "detail_extra_high": 0,
    "detail_medium": 0,
    "detail_extra_low": 0,
}
