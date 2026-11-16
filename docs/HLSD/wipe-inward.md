# Wipe inward — High Level Design

## Purpose and scope

Wipe inward reduces reheating of fresh plastic and visible seam artifacts by
moving the hot nozzle toward adjacent printed material during the external-wall
wipe. Wipe marks are especially visible at layer heights below 0.1 mm.
The option applies only to wipes after external walls, including walls around
holes. It does not offset wipes after inner walls, infill or supports. For an
outer contour the move is inward; for a hole it is away from the hole, toward
the surrounding material. The path must remain supported by material that is
already present when the wipe executes.

The operation belongs to G-code generation. It uses extrusion paths, their actual
widths and their print order. Changing its settings invalidates G-code export
while preserving the sliced geometry.

## Settings and eligibility

`wipe_inward` defaults to disabled and requires Wipe while retracting to be
enabled for the active filament. `wipe_inward_distance` defaults to 50% of the
actual external-wall extrusion width; it also accepts an absolute distance in
millimeters. Using the path width makes Auto width and Arachne's variable widths
meaningful. The effective offset is limited by that width and the spacing to the
adjacent wall. A zero distance disables the offset.

Only external perimeters with an adjacent inner perimeter are eligible. A
configured wall count alone cannot establish eligibility: the local geometry
may contain fewer walls. With Inner/Outer order the adjacent wall must already
be printed. Walls scheduled later never support the path.

Outer/Inner and Inner/Outer/Inner wall orders print the adjacent inner wall
after the external wall. Its geometry still identifies the inward side and the
presence of an adjacent wall, but an inward wipe can then use the external
bead's own material only up to half its actual width. Larger requested offsets
are limited to this half-width, even if a more distant inner perimeter was
printed earlier: the third wall in Inner/Outer/Inner order, or the inner wall
of another loop across a thin section in Outer/Inner order. The limit follows
the configured order, so it also applies on the first layer, where sandwich
order is not used.
It applies to percentages and equivalent absolute distances, using the actual
extrusion width rather than the configured Auto width.

Retraction and pressure advance calibrations disable inward wiping so it cannot
mask the behavior being measured. The calibration settings turn it off, and
G-code generation enforces this even if a profile or object override enables it.

## Path selection and support

The planner identifies an adjacent inner perimeter on the material side of the
outgoing wall. Contour winding and the distinction between outer contours and
holes establish a preferred direction; local printed geometry resolves ambiguous
or self-touching contours. Sampling farther along the wall identifies its adjacent
branch, while the available offset is measured from a nearby support point at the
extrusion endpoint. This local search has the same bounded corner reach as support
validation; a displacement measured at the sample cannot substitute for local support.

Candidate paths offset or translate the portion needed for the configured wipe
distance. A wide seam gap can prevent a supported forward path; following the
incoming printed wall backwards is also a candidate. If translating that wall
cannot provide a complete wipe around a curve, the planner tries an offset of
the reversed wall. Direction checks allow coordinate-rounding error at a
perpendicular entry, while rejecting actual backtracking. The planner checks the
complete executable path, including its connector from the nozzle position,
against the current and earlier printed perimeters. Nearby endpoints alone do
not establish support across a gap.

Each region of an island accumulates its printed perimeter prefix once, in
extrusion order. Every entity contributes its geometry only after it is printed,
and the prefix is discarded when that region of the island ends. This collection
is skipped when inward wiping is disabled or its configured distance is zero.
With Outer/Inner and Inner/Outer/Inner order, all inner walls of the region are
collected as targets before printing; they do not support the path until printed.
A mixed inner-wall loop remains an eligible target even when its first path is
an overhang: ordinary inner-wall
paths elsewhere in the loop identify it. Likewise, an external loop with an
overhanging start remains eligible when other segments identify the external
wall. It is available for support checks but is not an inner-wall target.
Candidate-specific support filtering and AABB trees are built only for eligible
external loops, then reused across their candidate paths. They contain only the
printed walls within the wipe length plus twice the corner reach of the support
checks around the seam, so an island with many holes does not rebuild trees of
its whole printed prefix for every loop.

Material-side validation applies with or without a seam gap. Along each
candidate, local wall normals point toward the adjacent inner wall;
samples on the opposite side are rejected even when they remain close enough
to the external wall to pass the support check. This uses the open wall geometry
without treating it as a closed polygon. Full paths at a zero-gap seam also
retain clearance from the external wall after their initial connector. At a
clipped corner, another branch can be closer than the requested offset, so
material-side and support checks apply without that additional clearance rule.

An accepted candidate replaces the stored wipe path as a whole. A short direct
inward move is also eligible when longer candidates fail validation. It may
waive full wall clearance, but must pass the material-side check. Every candidate's
initial direction is checked from the actual nozzle position, including the seam
gap and any loop pre-move; its construction origin is retained separately for
intersection checks. The short direct move takes
priority over the alternate offset when the preferred and translated paths
are unusable. A longer reversed path may replace the selected candidate only
when its distance to the target inner wall is no worse within tolerance.

## Fallback to the regular wipe

The original wipe path is retained when:

- The external wall has no suitable adjacent inner wall near the seam, as in
  single-wall areas or where inner walls are locally missing. With Inner/Outer
  order it must already be printed. A distant wall or a wall on the air side
  does not qualify.
- The requested or available offset, or the configured wipe distance, is zero
  or too small at the geometry's coordinate precision.
- Degenerate geometry prevents construction of a usable candidate, or all
  candidates fail the checks for printed support, direction, wall clearance or
  the connector from the actual nozzle position. This can occur at tight corners,
  narrow features or seam gaps.

Corners and seam gaps do not automatically trigger fallback: an offset,
translated, reversed or short direct inward path may still be valid. The regular
wipe is retained only when no candidate is accepted.

Fallback uses the path and retraction rules for `wipe_inward` disabled.
Wipe while retracting must still be enabled for a wipe to occur; `wipe_on_loops`
remains controlled by its own setting.

## Interaction with Wipe on loop

`wipe_on_loops` is an independent option that makes a short move before leaving
an external loop. It can operate with `wipe_inward` disabled. When both options
are enabled, its destination is the starting position for the inward wipe.

The loop move samples the outgoing and incoming paths by distance across path
boundaries. The sampling distance is bounded by the nozzle diameter and one
quarter of the total path length. It samples the outgoing path at up to 20% of
the nozzle diameter and rotates that point around the seam through one third
of the material-side corner angle. For a closed square outer contour, this
produces a move of 20% of the nozzle diameter at 30 degrees into the corner.
Coincident samples or degenerate angles suppress the move.

The nozzle position stored by G-code generation must match the emitted loop
move. Both travel planning and wipe execution depend on this position, including
when Wipe inward is disabled.

With a seam gap, a loop move may advance past the inward offset's original entry.
If that alone makes the connector backtrack, the entry advances to the nozzle's
projection on the offset. The planner extends the source as needed to preserve
the configured wipe length and validates the new connector and complete path.
Joins that already backtrack across the seam gap are not adjusted this way.

## Execution and retraction

Regular and inward wipe paths use a sentinel first point, replaced by the
actual nozzle position during execution. Selecting an inward path changes only
the stored wipe geometry. Seam selection and travel planning retain the actual
post-extrusion position, including any Wipe on loop move.

The next travel applies the normal minimum-travel threshold and retraction/lift
rules from that position. If retraction is needed, the ordinary retract operation
executes the selected wipe with the configured before/during/after split, then
replans travel from the wipe's endpoint when necessary. If a regular wipe would
be skipped, the inward wipe is skipped too. In particular, a short travel or an
extrusion starting at the current position does not execute an inward move,
retract or Z-hop. A consumed or replaced path is cleared so it cannot execute
twice. Layer and tool-change retractions can also consume the stored path.

Retraction is divided into portions before, during and after wiping. The amount
that can be retracted during the wipe depends on its executable length, wipe
speed and the active filament's retraction speed. Fractional retraction speeds
are retained in this calculation. For a 2 mm wipe at 100 mm/s and a retraction
speed of 25.5 mm/s, the wipe can retract 0.51 mm. With a total retraction of 0.8 mm
and both before/after percentages set to zero, the remaining 0.29 mm is retracted
before wiping. This split applies to regular and inward wipes whenever normal
travel planning or an explicit layer/tool-change retraction calls for a retract.
A travel that needs no retraction does not execute either wipe geometry,
irrespective of the configured retraction percentages.

## Implementation and verification

- [GCode.cpp](../../src/libslic3r/GCode.cpp) integrates path selection, nozzle
  position and retraction; [Print.cpp](../../src/libslic3r/Print.cpp) controls
  invalidation, and [PrintConfig.cpp](../../src/libslic3r/PrintConfig.cpp) defines
  the settings.
- [WipePathHelpers](../../src/libslic3r/GCode/WipePathHelpers.hpp) implements path
  sampling, offset selection and support checks.
- [Geometry tests](../../tests/libslic3r/test_wipe_path.cpp) cover support,
  degenerate paths, contour and hole orientations, and exact loop-move geometry
  across path subdivisions.
- [FFF tests](../../tests/fff_print/test_wipe.cpp) cover emitted trajectories,
  fallback, minimum-travel retraction and Z-hop rules, and export invalidation.
  Inward-wipe checks cover before/during/after retraction splits, including 100%
  before or after wiping, with both E modes and Wipe on loop. Short travels
  skip the wipe, retraction and Z-hop for every split. A threshold between the
  original travel length and the distance from an inward wipe's endpoint also
  verifies that selecting the inward path cannot create a retraction.
  With Wipe inward disabled, they check the loop move's direction and magnitude
  for Classic and Arachne, the subsequent wipe's start and length, and fractional
  retraction splitting in absolute and relative E modes.
  Loop-move checks use reserved role/wipe markers and extrusion state, and run
  with human-readable G-code comments both enabled and disabled.
