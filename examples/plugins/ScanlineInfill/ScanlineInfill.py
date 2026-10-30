# /// script
# requires-python = ">=3.12"
# dependencies = []
#
# [tool.orcaslicer.plugin]
# name = "Scanline Hatch Infill"
# description = "Example sparse infill plugin: straight scanlines with alternating direction."
# author = "OrcaSlicer contributors"
# version = "0.1.0"
# type = "infill"
# ///
"""Scanline hatch infill -- a small, working infill plugin.

The host calls generate() once per surface island with a value snapshot of the
Fill request. Paths returned here are open centre lines in scaled integer
coordinates; the host converts them to extrusions, applies flow and gap fill,
and does not clip them again. Every segment is therefore clipped against the
request expolygon here, holes included.

The pattern is a plain hatch: parallel lines at right angles to
request.direction_angle (Fill's built-in convention: fillers rotate by
-direction_angle and slice vertical lines), spaced request.spacing apart
(falling back to flow_width), with the grid phase anchored at
request.direction_origin. Consecutive lines alternate direction so the nozzle
generally ends a line near where the next one starts.
"""

import math

import orca

PATTERN_NAME = "scanline-hatch"


def _ring(polygon):
    """Return a host Polygon's distinct vertices as (x, y) integer pairs."""
    points = []
    for point in polygon.points:
        xy = (int(point.x), int(point.y))
        if not points or points[-1] != xy:
            points.append(xy)
    # Clipper rings normally repeat the first vertex. The closing edge is added
    # explicitly below, so drop a repeated endpoint rather than counting it twice.
    if len(points) > 1 and points[0] == points[-1]:
        points.pop()
    return points


def _project(ring, ux, uy, nx, ny, ox, oy):
    """Project a ring into (along-direction, normal) coordinates about the origin."""
    return [((x - ox) * ux + (y - oy) * uy,
             (x - ox) * nx + (y - oy) * ny) for x, y in ring]


def _interior_spans(rings, offset):
    """Return the spans of the line normal == offset that lie inside the region.

    All rings (contour and holes) are treated with the even-odd rule, so a hole
    splits a span in two. The half-open crossing test counts each edge once and
    makes tangent vertices contribute only zero-length spans.
    """
    cuts = []
    for ring in rings:
        for i in range(len(ring)):
            u0, n0 = ring[i - 1]
            u1, n1 = ring[i]
            if (n0 > offset) == (n1 > offset):
                continue
            cuts.append(u0 + (offset - n0) * (u1 - u0) / (n1 - n0))

    cuts.sort()
    # Even-odd: consecutive crossings bound one interior span.
    return [(cuts[i], cuts[i + 1]) for i in range(0, len(cuts) - 1, 2)
            if cuts[i + 1] > cuts[i]]


class ScanlineHatch(orca.infill.InfillPluginCapabilityBase):
    def get_name(self):
        # Stable, nonlocalized key: sparse_infill_pattern stores this verbatim.
        return PATTERN_NAME

    def no_sort(self):
        # generate() emits paths in snake order; keep that order instead of
        # letting the host re-sort them into long travels.
        return True

    def generate(self, request):
        try:
            expolygon = request.expolygon
            rings = [_ring(expolygon.contour)]
            rings.extend(_ring(hole) for hole in expolygon.holes)
        except (AttributeError, TypeError, ValueError):
            return []

        # Fewer than three vertices cannot enclose an area.
        rings = [ring for ring in rings if len(ring) >= 3]
        if not rings:
            return []

        density = float(request.density)
        if not math.isfinite(density) or density <= 0.0:
            return []

        # FillLine uses spacing / density for sparse lines; match that convention.
        spacing_mm = float(request.spacing) / density
        if not math.isfinite(spacing_mm) or spacing_mm <= 0.0:
            spacing_mm = float(request.flow_width)

        # Millimetres -> scaled integer units via the live scale, never 1e6.
        unit_mm = float(orca.slicing.unscale(1))
        if (not math.isfinite(spacing_mm) or spacing_mm <= 0.0
                or not math.isfinite(unit_mm) or unit_mm <= 0.0):
            return []
        spacing = max(1, int(round(spacing_mm / unit_mm)))

        angle = float(request.direction_angle)
        if not math.isfinite(angle):
            angle = 0.0
        # Fill::_infill_direction() returns the angle by which a built-in filler
        # rotates the region (by -angle) to slice vertical lines. The actual
        # scanline direction is therefore angle + 90 degrees, and the lines are
        # spaced along the angle vector.
        ux, uy = -math.sin(angle), math.cos(angle)
        nx, ny = math.cos(angle), math.sin(angle)

        origin = request.direction_origin
        ox, oy = int(origin.x), int(origin.y)

        projected = [_project(ring, ux, uy, nx, ny, ox, oy) for ring in rings]
        normal_min = min(n for ring in projected for _, n in ring)
        normal_max = max(n for ring in projected for _, n in ring)

        # Lines are k * spacing in the normal direction, anchored at the origin.
        first_line = math.ceil(normal_min / spacing)
        last_line = math.floor(normal_max / spacing)

        paths = []
        emitted_lines = 0
        for line_index in range(first_line, last_line + 1):
            offset = line_index * spacing
            spans = _interior_spans(projected, offset)
            if not spans:
                continue

            # Alternate only lines that produced geometry, so a line arriving
            # through a hole or notch does not break the snake unnecessarily.
            reverse = emitted_lines % 2 == 1
            emitted_lines += 1
            if reverse:
                spans.reverse()

            for span_start, span_end in spans:
                if reverse:
                    span_start, span_end = span_end, span_start

                path = []
                for along in (span_start, span_end):
                    # Back from (u, offset) to the original scaled coordinates.
                    x = int(round(ox + along * ux + offset * nx))
                    y = int(round(oy + along * uy + offset * ny))
                    if not path or path[-1] != (x, y):
                        path.append((x, y))

                if len(path) >= 2:
                    paths.append([orca.host.Point(x, y) for x, y in path])

        return paths


@orca.plugin
class ScanlineInfillPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(ScanlineHatch)
