# Overhang seam preview — High Level Design

## Problem

The preview draws a seam marker where each outer wall loop starts and ends. The
marker comes from the seam detector of `GCodeProcessor`, which reads the G-code
text: it opens a candidate on the first "Outer wall" extrusion and closes it on
the first move that no longer continues the wall.

An outer wall that starts on an overhang is labelled "Overhang wall", exactly
like the overhanging part of an inner wall, so the detector never opens a
candidate for it, although the printed seam is there. Whether a loop is an outer
wall is known only to the G-code generator.

## Design

`GCode::extrude_loop` flags a loop that is not in spiral vase mode, is an outer
wall (`inset_idx` 0, or any of its paths before the seam gap clipping is an
external perimeter) and whose first path is an overhang. `GCode::_extrude_piece`
then writes one comment, `; OUTER_OVERHANG_WALL`, next to the feature tag of the
first path, after the travel and before the `PA_CHANGE` tag.

`GCodeProcessor` keeps two flags. The tag sets "pending". The next printing move
clears it and, if that move is an "Overhang wall", sets "candidate". While
"candidate" is set, "Overhang wall" moves count as "Outer wall" for the detector
only; the flag is cleared when the detector closes the candidate. Continuation
and closing already treat both roles alike, so the marker's position, threshold
and timing come from the existing detector, and the moves keep their real role.

The tag is in the file, so G-code opened from disk, stored plate G-code and the
BBL post-processing reload get the same markers as a fresh slice.

## Limits

- G-code files written by earlier versions have no tag and show no marker for
  these loops, as before.
- The tag adds one comment line per such loop. `PressureEqualizer` counts it in
  its 128-line look-back, so with extrusion rate smoothing a feedrate near that
  limit can differ slightly. Other filters pass the line through.
- Spiral vase prints are excluded.
- The existing detector's limits stay: loops printed with no move between them
  share one candidate, and a gap of 0.25 mm or more gives no marker.

## Tests

`tests/fff_print/test_gcodeprocessor.cpp`, tag `[OverhangSeam]`: markers against
the same stream labelled "Outer wall" in each detector state; a following wall
stays unmarked; and an exported sphere against a replay of its G-code relabelled
"Outer wall".
