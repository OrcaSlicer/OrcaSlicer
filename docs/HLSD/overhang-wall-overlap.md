# Overhang wall overlap

`overhang_wall_overlap` adjusts spacing between adjacent Arachne walls near
unsupported regions. It is disabled at zero and requires overhang detection
and lower-layer slices. The outermost wall stays fixed; each subsequent wall
inherits displacement from its outer neighbor and may add extra overlap of
its own. The zero-width inner-contour marker inherits displacement without
adding overlap, keeping the fill boundary aligned with the adjusted walls.

## Support and distance

`WallToolPaths::applyOverhangWallOverlap()` operates after stitching and short
line removal, before inner-contour extraction and simplification. For each
wall pair it measures support at the original outer wall, using its original
interpolated width and signed distance to the lower layer's printable slices.
The unsupported fraction is `clamp(0.5 + signed_distance / width, 0, 1)`.
Moving walls does not change the support classification of subsequent pairs.

Original wall segments are stored as `ThickLine` objects, keeping geometry and
endpoint widths together in the AABB index. Support samples carry arc length
and unsupported fraction; overlap control points carry arc length and overlap
factor. These are private types within `WallToolPaths.cpp`.

The original inner wall is sampled at intervals no larger than 0.2 mm.
Distances for transitions are arc lengths along that unmodified wall, not XY
distances between points on different loops. Zero-unsupported, start and full
threshold crossings are interpolated between samples. Negative support
estimates are retained until the zero crossings have been located.

## Transition profile

The start threshold identifies connected regions that require adjustment;
it is not a zero-overlap constraint. Each entry and exit extends outwards
from its start-threshold crossing by at most the maximum transition length,
stopping sooner at the first fully supported point. That endpoint has zero
extra overlap. The maximum extension is five inner-wall nozzle diameters per end,
derived from the perimeter flow rather than the local Arachne line width.

Control points describe a piecewise linear overlap profile. An entry connects
its zero endpoint to the first full-threshold point, and an exit connects the
last full-threshold point to its zero endpoint. Between those target points,
the original support-based overlap factors are retained. If a region never
reaches full, its first and last maximum-support-deficit points are the targets,
with factor `(peak - start) / (full - start)`. A full threshold no greater than
start gives target factor one while retaining spatial entry and exit ramps.

When neighboring extensions overlap, their shared control point is the lowest
unsupported fraction within the interval reachable by both extensions. A
continuous minimum plateau uses its arc-length midpoint. Among disconnected
equal minima, the plateau closest to the shared interval's midpoint is chosen.
The shared overlap factor is the arithmetic mean of the two independent ramps
evaluated there. Both ramps are then connected to that control point. The
handoff need not have zero overlap. Nonintersecting extensions retain their
individual zero endpoints, with zero overlap between them.

Closed walls are evaluated periodically, so the stored first vertex is not a
transition boundary. A loop that exceeds start everywhere has no entry or exit
and retains its local support-based factors. Real open endpoints stop extension
without forcing an already active endpoint to zero. Missing usable outer-wall
geometry stops extension; separate wall paths never share a profile. Odd middle
lines remain excluded from adjustment.

Profile control points are inserted into the wall geometry before displacement,
so ramp endpoints and handoffs are not snapped to the sampling grid. The profile
is continuous and piecewise linear; its slope need not be continuous. Existing
wall simplification and integer-coordinate rounding still apply afterwards.

The profile builder separates threshold insertion, region detection and
neighboring-transition merging. Geometry sampling, control-point insertion and
wall displacement are separate operations. `applyOverhangWallOverlap()` owns
the original geometry snapshots and processes wall pairs from outside to inside;
the helpers do not access or mutate other wall pairs.

## Verification

`tests/libslic3r/test_arachne_walls.cpp` exercises this through the public wall
generator, measuring generated wall positions against an unadjusted reference.
The overhang-overlap cases cover untriggered slopes, entry extension below start,
fully supported stops, maximum and partial overlap, inherited inner-wall displacement,
and neighboring ramps that either merge or remain separate. An unequal-ramp case
checks averaging at a minimum plateau against independently generated ramps.
These cases do not require exposing the private profile builder.
