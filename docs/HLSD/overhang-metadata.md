# Overhang metadata and the Overhang preview — High Level Design

## Problem

The preview can colour lines by speed, width or fan, but not by how much of a line hangs over nothing. The slicer
knows this while it generates the G-code: the overhang speed estimator measures every wall against the layer below.
The preview only has the G-code text, so the measurement has to travel inside the file, without changing anything the
printer executes.

## Option and invariants

`gcode_overhangs` (Print settings, off by default, invalidates only the G-code export) turns the metadata on.

- Off: the G-code is the same as without this feature.
- On: only comment lines and inline comment markers are added. Speeds, fan, pressure advance, move order and move
  splitting stay the same, so the printer commands match the option off. The metadata uses its own estimators
  (`GCode::m_wall_overhang_estimator`, `GCode::m_infill_overhang_estimator`) and never prepares the speed and fan
  estimator. It only takes the outlines computed ahead for a layer from that estimator's precomputed data.

## What is measured

All values are percentages of the bead width, measured in XY against the layer below.

- Inner, outer and overhang walls: how far the bead's outer edge reaches past the lower layer's slice outline
  (`lslices`), less the part of the bead past its own layer's outline; before these corrections it is 100 minus the
  overlap the speed estimator uses. A wall may read past 100 %, up to `MAX_OVERHANG_PERCENTAGE` (1000 %, ten widths).
  - A wall within about half a width of its own layer's outline reads at most how far that outline moved from the
    layer below, so a wall over an identical outline reads 0 %. If it still reads a full width or more, it reads the
    cap unless the layer below has an edge of its own under the wall: one the wall's inward normal meets within ten
    widths, before it leaves the layer's material. So the side of a shelf, or an island across a gap from the body,
    reads horizontal. Deeper walls read how far they reach past the nearest lower edge.
  - When the nearest lower edge faces away from the nearest edge of the wall's own layer (their directions differ by
    more than 90°), as the far side of a hole or of a thin wall does, neither rule above applies: the wall reads how
    far it reaches past that edge, and the cap from a full width on.
- Gap fill, infill, solid infill and bridges: the share of the bead's cross-section (the segment one width long across
  the path) that has no material printed on the layer below under it, up to 100 %. The material is the footprint of the
  lower layer's walls and fills. Combined infill is measured against the layer under its bottom.
- Supports, skirt, brim, wipe tower, ironing and custom extrusions are not measured: they read N/A
  (`GCodeProcessor::NO_OVERHANG_PERCENTAGE`, -1). `GCodeProcessor::measures_overhang()` is the one predicate for the
  measured roles.
- The first layer and the first object layer over a raft read 0 %.

A long move carries a local profile: equal cells along a line or samples along a fitted arc, so a line that bridges
only in its middle is coloured only there. A cell is at most a quarter bead or 0.05 mm long, whichever is more, for
gap fill, infill and bridges, and at most a bead or 0.1 mm, whichever is more, for walls. Arc samples are at most a
bead or 0.1 mm apart, whichever is more, for every role.

## G-code tags

Both tag dialects are supported: `; OVERHANG: ` for Bambu-style G-code, `;OVERHANG:` for the others
(`GCodeProcessor::Reserved_Tags` and `Reserved_Tags_compatible`).

- `OVERHANG: <percentage>`: the scalar value of the following moves, rounded to one decimal, written when it changes
  and again on entering each measured role. A move with a profile carries its largest cell or sample.
- `OVERHANG_Z_DISTANCE: <mm>`: the distance between the slice planes of the layer and its reference layer, the
  denominator of the angle. Zero, or a file without it, falls back to the extrusion height.
- `OVERHANG_LINE: <total>,<offset>,<values>` and `OVERHANG_ARC: ...`: a profile in chunks, each a comment line with
  about 96 characters of values. `<total>` is the number of cells of a line or of samples along an arc (2 to 65536),
  `<offset>` the first one in the chunk, and each value is `<percentage>` or `<percentage>*<count>` for a run of equal
  ones. Chunks arrive in order and must cover the whole profile.
- `OVERHANG_LINE_APPLY` / `OVERHANG_ARC_APPLY`: an inline marker, after a space, on the motion the profile belongs
  to. A profile applies only to the next move, and only when that move carries the marker of its kind; any other move
  drops it. The moves are G0 to G3 and the commands the processor runs as G1 (firmware retraction G10 and G11, and
  G28); feedrate-only lines are skipped, and other commands, such as G29, leave the profile pending. A scalar
  `OVERHANG:` tag drops it too. So a removed or inserted move cannot carry a profile to an unmarked one. The marker
  does not name its move, though: a marked move moved after another move's profile reads that profile.

Uniform data writes only the scalar tag. The pressure equalizer and the fan mover keep the marker on every piece of a
move they split, and the processor extends the profile over the collinear pieces. The spiral vase changes lines in
place and keeps their comments.

## Processor and preview

`GCodeProcessor` validates every number, run and chunk. An invalid profile falls back to the scalar value, and an
invalid scalar tag reads 0 %. It stores profiles as runs (`GCodeProcessorResult::OverhangRuns`), so a crafted
`value*count` cannot allocate per cell. `has_overhang_metadata` is set by a valid scalar tag, or by a profile bound
to a stored extrusion. Each `MoveVertex` carries `overhang_percentage`, `overhang_z_distance` and
`overhang_profile_id`.

The preview (`libvgcode::EViewType::Overhang`) is offered only for G-code with metadata. It shows either the share in
percent (0 to 100 %) or an angle, `atan(0.01 * percentage * width / z_distance)`, on a linear or logarithmic scale.
N/A lines are grey. The position marker and the properties table read a line's largest value.

## Limitations

- Walls are compared with the model's slice outline, not with deposited material, and support does not count.
- Only the external wall's angle is the face angle. An inner wall reads the angle of its own outer edge, which is
  smaller; gap fill, infill and bridges read an equivalent angle derived from the unsupported width.
- The angle saturates in a step: a line measured up to a full bead (gap fill, infill, bridges) reads 90° once fully
  unsupported, a wall at the cap. Walls past 100 % all read 100 % in percent.
- The logarithmic scale spreads small values over most colours, so steep angles share the last ones.
- The material footprint infill is measured against ends flat at each lower bead's end, half a width short of the
  bead's round end.
- A wall over a gap in the lower outline ramps to 100 % within a width of each edge of the gap, while infill over the
  same gap reads the share of its cross-section with no material below: 100 % only where the whole section misses the
  material, 50 % for a bead running along the middle of a gap half its width. The two definitions differ there.
- A fitted arc shows the largest value of each planner chord it is drawn with.
- Every move of every G-code in the preview is 12 bytes larger in the processor result and 8 bytes larger in
  `libvgcode::PathVertex`, with or without metadata. The metadata makes the G-code larger and slicing slower.
