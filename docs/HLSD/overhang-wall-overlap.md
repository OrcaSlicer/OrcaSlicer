# Overhang wall overlap

## Overview

This option increases overlap between adjacent walls in overhang areas to help
wall-to-wall bonding. The outermost wall stays in place; inner walls move closer
to their outer neighbors, with gradual transitions into and out of adjusted areas.
The fill boundary follows the inner walls.

It requires Arachne and **Detect overhang walls**. The extra overlap limit defaults
to **0% (disabled)**. The activation threshold selects overhang regions to adjust;
the full threshold sets where overlap reaches its limit. Transitions can extend
below the activation threshold, but stop at fully supported areas.

## Implementation reference

> Compiled by AI from the current code, primarily for AI agents maintaining this feature.
> This section describes implementation details, not additional design requirements.
> Keep it aligned with the referenced code when behavior changes.

### Configuration and integration

- [PrintConfig.cpp](../../src/libslic3r/PrintConfig.cpp) defines the percent options:
  `overhang_wall_overlap` (0–20, default 0), `overhang_wall_overlap_start`
  (0–100, default 25), and `overhang_wall_overlap_full` (0–100, default 75).
- [PerimeterGenerator.cpp](../../src/libslic3r/PerimeterGenerator.cpp) passes them
  as fractions only with overhang detection, a positive limit, non-null lower
  slices, and `layer_id > raft_layers`. Maximum transition extension per end is
  `5 * perimeter_flow.nozzle_diameter()` in mm. A separate inner-wall pass receives
  the existing outer wall as `outer_wall_anchor`, keeping the actual outermost wall fixed.
- [WallToolPaths.cpp](../../src/libslic3r/Arachne/WallToolPaths.cpp):
  `applyOverhangWallOverlap()` runs after stitching and short-line removal, before
  inner-contour extraction and simplification. It owns original wall snapshots
  and processes wall pairs from outside to inside. Helpers and profile types are
  private to this file; the steps below refer to these helpers.

### Support and transition profile

1. `sample_overlap_wall()` subdivides original segments at nominal intervals no
   greater than 0.2 mm. `sample_wall_support()` measures support at the nearest
   original outer-wall point, using its interpolated width and signed distance
   to lower-layer slices: `unsupported = min(1, 0.5 + distance / width)`.
   Negative values are retained for zero-crossing interpolation; unavailable
   or out-of-reach wall pairs yield zero. Positions are original-wall arc lengths.
2. `make_overlap_profile()` inserts zero/start/full crossings, finds regions
   strictly above start, and merges neighboring transitions. Each region extends
   at most the configured length beyond its start crossings, stopping at zero
   support deficit or a path endpoint. Entry/exit ramps connect to the first/last
   full-threshold points, or peak points when full is not reached. Between those
   targets, factors follow `clamp((unsupported - start) / (full - start), 0, 1)`;
   when `full <= start`, triggered regions target one with entry/exit ramps retained.
3. `merge_overlap_transitions()` joins overlapping extensions at the lowest
   unsupported fraction in their shared interval. It chooses a minimum plateau's
   midpoint, preferring the plateau nearest the interval midpoint when tied.
   The shared factor averages the two ramps evaluated there; it need not be zero.
4. `wall_overlap_profile()` handles closed paths using three periods and keeps
   the middle period. A loop entirely above start uses local factors directly.
   Open endpoints stop extension without forcing active overlap to zero.
   Separate paths have separate profiles.

### Displacement

`apply_overlap_profile()` inserts profile control points into the wall geometry.
The scalar profile is continuous and piecewise linear, without slope continuity.
For each wall pair, the extra-overlap limit is the configured fraction times the
smaller nominal wall spacing. The pairing reach is the mean spacing plus cumulative
extra overlap plus 0.5 mm; missing wall geometry resets accumulation.

`move_wall_towards_outer()` combines inherited outer-wall displacement and local
extra overlap, clamping the scalar shift to the cumulative limit. It moves toward
the current outer wall, using a miter of connected outer edges at eligible corners.
Odd middle lines are skipped. The zero-width inner-contour marker only inherits
displacement, without adding overlap. Coordinate rounding and subsequent wall
simplification still apply.

### Test coverage

[test_arachne_walls.cpp](../../tests/libslic3r/test_arachne_walls.cpp) contains five
`[OverhangOverlap]` cases exercising the public wall generator: triggering,
fully supported stops, merged/separate transitions, partial peak overlap, and
unequal-ramp averaging. Assertions also cover a fixed outer wall, inherited inner
displacement, and closed endpoints. These describe test coverage, not execution results.
