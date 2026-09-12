# Wipe inward — High Level Design

## Purpose and scope

Wipe inward reduces visible seam artifacts by moving the external-wall wipe
toward adjacent printed material. For an outer contour this is an inward move;
for a hole it is a move away from the hole. The path must remain supported by
material that is already present when the wipe executes.

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

Only external perimeters with a suitable, previously printed inner perimeter
are eligible. A configured wall count alone cannot establish eligibility:
the local geometry may contain fewer walls, and walls scheduled later do not
provide support. Outer/Inner wall order therefore normally retains the regular
wipe path.

## Path selection and support

The planner identifies an adjacent inner perimeter on the material side of the
outgoing wall. Contour winding and the distinction between outer contours and
holes establish a preferred direction; local printed geometry resolves ambiguous
or self-touching contours.

Candidate paths offset or translate the portion needed for the configured wipe
distance. A wide seam gap can prevent a supported forward path; following the
incoming printed wall backwards is also a candidate. The planner checks the
complete executable path, including its connector from the nozzle position,
against the current and earlier printed perimeters. Nearby endpoints alone do
not establish support across a gap.

An accepted candidate replaces the stored wipe path as a whole. If no supported
candidate exists, the regular path is retained. This fallback covers missing
adjacent walls, degenerate geometry and unsupported connectors.

## Interaction with Wipe on loop

`wipe_on_loops` is an independent option that makes a short move before leaving
an external loop. It can operate with `wipe_inward` disabled. When both options
are enabled, its destination is the starting position for the deferred wipe.

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

## Deferred execution and retraction

The stored wipe path uses a sentinel first point. Execution starts from the
actual nozzle position and proceeds to the second stored point. Path selection,
support validation and wipe-length calculation must all use this same executable
geometry, especially after a Wipe on loop move.

Accepting an inward path marks the deferred wipe as requiring retraction. This
ensures that a short travel to the next wall, including an unchanged XY position,
does not discard the accepted path through the normal minimum-travel check.

Retraction is divided into portions before, during and after wiping. The amount
that can be retracted during the wipe depends on its executable length, wipe
speed and the active filament's retraction speed. Fractional retraction speeds
are retained in this calculation. For a 2 mm wipe at 100 mm/s and a retraction
speed of 25.5 mm/s, the wipe can retract 0.51 mm. With a total retraction of 0.8 mm
and both before/after percentages set to zero, the remaining 0.29 mm is retracted
before wiping. This rule applies with Wipe inward enabled or disabled.

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
  fallback, deferred retraction and export invalidation. With Wipe inward
  disabled, they check the loop move's direction and magnitude for Classic and
  Arachne, the subsequent wipe's start and length, and fractional retraction
  splitting in absolute and relative E modes.
