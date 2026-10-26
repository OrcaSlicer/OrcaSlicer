# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Fuzzy Slices"
# description = "Applies the fuzzy-skin jitter to the slice contours themselves at the Slice boundary (demo)."
# author = "OrcaSlicer"
# version = "0.01"
# type = "slicing-pipeline"
# ///
"""Fuzzy Slices -- the fuzzy-skin effect applied at slice time.

Orca's built-in fuzzy skin perturbs the outer-wall EXTRUSION PATHS during
perimeter generation, so only the printed wall is fuzzy. This sample instead
perturbs the sliced outline itself at Step.posSlice, using the same
resample-and-jitter algorithm as libslic3r's fuzzy_polyline (uniform noise):
walk each ring, drop a new vertex every 3/4..5/4 * point_distance_mm of
perimeter, and displace it by a random +/- thickness_mm along the segment
normal. Because the slice contour itself changes, everything derived from it
(perimeters, infill boundaries, overhang detection) inherits the noise and
the fuzz shows in the toolpath preview.

Mechanically this demonstrates the count-CHANGING mutation idiom: a fuzzed
ring has a different vertex count, so it is rebuilt as a fresh
orca.host.Polygon (append() per vertex) and written back by assigning
ex.contour / calling ex.set_holes() on the live ExPolygon. The in-place edit
persists through the surface collection and leaves surface types untouched;
layer.make_slices() then re-derives the merged islands. Compare the Inset
sample (whole-surface offset + slices.set) and Twistify (count-preserving
in-place transforms).

The jitter preserves vertex order, so the contour keeps its CCW winding
(contour assignment does not re-normalize); set_holes() re-normalizes holes
to CW. The RNG is seeded per layer, so re-slicing reproduces the same fuzz.
The first layer is skipped by default for bed adhesion (like the built-in
fuzzy_skin_first_layer = off). No numpy required; for very dense models the
Polygon.as_array()/set_points numpy path would be the faster route.
"""
import math
import random
import json

import orca

_DEFAULTS = {
    "thickness_mm":      0.3,   # max normal displacement (built-in fuzzy_skin_thickness default)
    "point_distance_mm": 0.8,   # target resample spacing (built-in fuzzy_skin_point_dist default)
    "fuzz_holes":        1.0,   # nonzero: jitter hole rings too, not just the outer contour
    "skip_first_layer":  1.0,   # nonzero: keep layer 0 crisp for bed adhesion
}


def _params(self):
    try:
        src = json.loads(self.get_config())
    except (AttributeError, TypeError):
        src = {}
    out = {}
    for key, default in _DEFAULTS.items():
        try:
            out[key] = float(src[key])
        except (KeyError, TypeError, ValueError):
            out[key] = default
    return out


def _fuzz_ring(points, thickness, min_dist, rand_range, rng):
    """Resample + jitter one closed ring (list of Point refs).

    Returns a new orca.host.Polygon, or None to keep the original ring (too
    small to resample). Mirrors libslic3r's fuzzy_polyline: new vertices every
    min_dist + rand*rand_range of arc length, each displaced +/-thickness
    along the segment's left-hand normal.
    """
    if len(points) < 3:
        return None
    out = []
    dist_left_over = rng.random() * (min_dist / 2.0)  # arc length before the first new vertex
    p0x = float(points[-1].x)
    p0y = float(points[-1].y)
    for p1 in points:
        p1x = float(p1.x)
        p1y = float(p1.y)
        dx = p1x - p0x
        dy = p1y - p0y
        seg = math.hypot(dx, dy)
        if seg > 0.0:
            d = dist_left_over
            while d < seg:
                t = d / seg
                r = (rng.random() * 2.0 - 1.0) * thickness
                out.append((p0x + dx * t - dy / seg * r,
                            p0y + dy * t + dx / seg * r))
                d += min_dist + rng.random() * rand_range
            dist_left_over = d - seg  # carry the remainder into the next segment
        p0x, p0y = p1x, p1y
    if len(out) < 3:
        return None  # ring shorter than ~2 resample steps: leave it crisp
    poly = orca.host.Polygon()
    for x, y in out:
        poly.append(orca.host.Point(int(round(x)), int(round(y))))
    return poly


class FuzzySlices(orca.slicing.SlicingPipelineCapabilityBase):
    def get_name(self):
        return "Fuzzy Slices"

    def get_default_config(self):
        return _DEFAULTS

    def execute(self, ctx):
        if ctx.step != orca.slicing.Step.posSlice or ctx.object is None:
            return orca.ExecutionResult.success()

        p = _params(self)
        if p["thickness_mm"] <= 0.0 or p["point_distance_mm"] <= 0.0:
            return orca.ExecutionResult.success("Fuzzy Slices: zero thickness/point distance, nothing to do")

        # Millimeters -> scaled integer units via the *live* scale (never hardcode 1e6).
        mm = 1.0 / orca.slicing.unscale(1)
        thickness = p["thickness_mm"] * mm
        # The spacing between new vertices varies between 3/4 and 5/4 the supplied
        # value, same as the built-in fuzzy skin.
        min_dist = p["point_distance_mm"] * mm * 0.75
        rand_range = p["point_distance_mm"] * mm * 0.5
        fuzz_holes = p["fuzz_holes"] != 0.0
        first = 1 if p["skip_first_layer"] != 0.0 else 0

        rings = 0
        layers_touched = 0
        for idx, layer in enumerate(ctx.object.layers()):
            if ctx.cancelled():
                break
            if idx < first:
                continue
            rng = random.Random(0x5EED + idx)  # per-layer seed: re-slices reproduce the same fuzz
            edited = False
            for region in layer.regions():
                for surface in region.slices.surfaces:
                    ex = surface.expolygon
                    contour = _fuzz_ring(ex.contour.points, thickness, min_dist, rand_range, rng)
                    if contour is not None:
                        ex.contour = contour  # vertex order preserved, so CCW winding survives
                        rings += 1
                        edited = True
                    if fuzz_holes and ex.holes:
                        new_holes = []
                        changed = False
                        for hole in ex.holes:
                            fuzzed = _fuzz_ring(hole.points, thickness, min_dist, rand_range, rng)
                            if fuzzed is not None:
                                new_holes.append(fuzzed)
                                changed = True
                                rings += 1
                            else:
                                new_holes.append(hole)  # untouched rings pass through unchanged
                        if changed:
                            ex.set_holes(new_holes)  # copies each ring and re-normalizes to CW
                            edited = True
            if edited:
                # Re-derive the merged islands from the fuzzed region slices.
                layer.make_slices()
                layers_touched += 1

        return orca.ExecutionResult.success(
            f"Fuzzy Slices: fuzzed {rings} ring(s) on {layers_touched} layer(s) "
            f"(+/-{p['thickness_mm']} mm @ {p['point_distance_mm']} mm)")


@orca.plugin
class FuzzySlicesPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(FuzzySlices)
