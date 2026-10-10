# Overhang seam preview — High Level Design

## Purpose and scope

The preview draws a seam marker where each outer wall loop starts and ends. The
marker comes from the seam detector of `GCodeProcessor`, which reads the G-code
text: it opens a candidate on the first "Outer wall" extrusion and closes it on
the first move that no longer continues the wall, placing the marker midway
between the two points when they are less than 0.25 mm apart.

An outer wall that starts on an overhang has no such marker. The G-code labels
an overhanging part of any wall "Overhang wall", outer or inner alike, so the
detector never opens a candidate for it, although the printed seam is there.
Treating every "Overhang wall" as an outer wall would mark inner walls too.
Whether a loop is an outer wall is known only to the G-code generator.

This feature gives such loops the marker the detector would place if the loop
were labelled "Outer wall". It does not change the G-code text, the G-code
filters, the extrusion roles shown in the preview or the seam the printer
prints.

A loop that starts on an overhang and has an "Outer wall" segment later is an
outer wall: only outer walls carry that label. The processor recognises it from
the text alone, so this also works for G-code opened from a file. A loop that
overhangs all the way round has no such segment; for it the generator's
hand-over below is needed, which a G-code file does not have.

## Hand-over beside the text

The generator passes the eligible loops to the processor outside the G-code
text. Text markers were rejected: the filters between generation and output
(pressure equalizer, fan mover, cooling, adaptive pressure advance) count and
rewrite lines, so even comments removed before writing would change the file.

`GCode::extrude_loop` hands over a loop when it is a wall of an object, the
print is not in spiral vase mode, the loop is an outer wall (`inset_idx` 0, or
any of its paths before the seam gap clipping is an external perimeter) and its
first printing move belongs to an overhang path. `GCodeWriter` records, while a
loop with an overhang path is being printed, the start of its first printing
move and the end of its last one, in the coordinates written to the text (plate
offset subtracted, quantized to the export precision) together with the
filament. A move counts as printing by the same rule the processor applies: the
emitted E grows at the single precision the G-code reader passes on, and X or Y
changes or the move is an arc.

The generator collects a layer's loops and publishes them as one packet when
the layer is finished. The export pipeline runs generation and output in
separate threads and generation may run ahead, so packets go through a small
locked channel owned by the processor and are keyed by the ordinal of the layer
change tag that opens the layer. The processor takes a packet when it reads
that tag. The lock is taken once per layer in each stage, never per move.

## Matching and the seam detector

On a path that starts with an "Overhang wall" printing move and is not handed
over, the processor keeps the start the detector would take, following a sloped
start up in Z. If an "Outer wall" printing move continues the path, the
detector candidate opens at that start, as on an outer wall from its first
move. Any other move drops it. Spiral vase prints keep the old detection.

For handed-over loops, on an "Overhang wall" printing move the processor looks
for a loop of the current packet that starts at the move's start with the same
filament, compared exactly on the export grid. A match makes the detector
candidate an outer wall for the detector only: the move opens or continues the
candidate exactly as an "Outer wall" move would, including following a sloped
start up in Z.

In both cases the real role of the moves is kept. Position, threshold and
timing of the marker come from the existing detector, so a matched loop gets
the marker an "Outer wall" loop would get, and none when that detector would
place none.

## Diagnostics

The match is a heuristic: a different loop of the same layer and filament that
starts and ends at the same points would be accepted. The processor therefore
checks what it can and reports discrepancies instead of hiding them:

- a loop whose start no move matched is missed;
- a matched loop whose candidate ended elsewhere, merged with another candidate
  (no move between two walls) or never ended is suspect;
- the generator and the processor count layer change tags; a difference means
  the stream had extra or missing tags, for example from custom G-code.

The counts are stored in `GCodeProcessorResult::overhang_seam_stats` and logged
under `[OverhangSeamPreview]`. When the feature handed over at least one loop
and found a discrepancy, the export adds one aggregated slicing warning listing
each affected object with its earliest affected layer and count. Agreement of
the checks is consistency, not proof that every marker belongs to the intended
loop.

As a conservative limit, loops whose quantized command start or any printing
endpoint has Z 0 are not handed over. With the usual zero origin this avoids
the processor's replacement of a zero Z by the layer height; a negative
`z_offset` can produce such coordinates. The command-space check is not
equivalent to that replacement when `G92` changes the Z origin; other
mismatches remain covered by the general diagnostics. A separate warning lists
the skipped loops' export layer numbers, saying the seam marker may not be
shown there if the seam falls on an overhang.

## Limits

- G-code opened from a file: a loop that overhangs all the way round has no
  marker; loops with an "Outer wall" segment have one. When the file has a
  path printed as "Overhang wall" from start to end, the preview says once per
  session that seam markers on overhangs are not shown for G-code files. Such a
  path may be an inner wall, so the notice can appear where no marker is due.
- Spiral vase prints are excluded entirely.
- The hand-over is conservatively disabled for writers requiring full XYZ
  emission (`has_axis_remap()`), including all belt configurations and
  non-identity Cartesian axis remaps. The capture does not generally reproduce
  their emitted machine coordinates. Mixed loops still use text recognition and
  the existing seam detector, with its limits.
- A sliced project restored from cache has no `inset_idx`; a loop that is an
  overhang all the way round is then not recognised as an outer wall.
- The existing detector's limits stay: loops printed with no move between them
  share one candidate, and a gap of 0.25 mm or more gives no marker.
- Persistent relative or inch positioning left by custom G-code is not
  supported.
- Loops with a command Z of 0 at their start or at a printing endpoint are not
  handed over (see Diagnostics).

## Implementation and verification

The hand-over and the matching live in `src/libslic3r/GCode/OverhangSeamLoops`;
the hooks are `GCodeWriter`'s extrusion capture, `GCode::extrude_loop`,
`GCode::process_layer` and `GCode::do_export` on the generator side and
`GCodeProcessor::process_G1` on the processor side. The notice for G-code files
is shown by `Plater::load_gcode`.

Tests in `tests/fff_print/test_gcodeprocessor.cpp` and `test_gcodewriter.cpp`
(tag `[OverhangSeam]`) compare the markers with the same stream labelled "Outer
wall" in each state of the detector and with a sloped start, check that the
writer's capture skips moves the reader cannot tell from travel, cover inner walls, every diagnostic case, the
notice flag and spiral vase, and compare an exported sphere's markers with a
replay of its G-code relabelled "Outer wall". Not covered by tests: `G92`, plate
and extruder offsets, arcs, multi-nozzle, sequential printing, the PA
calibration and command Z 0.
