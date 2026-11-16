# Travels that avoid crossing walls

`docs/HLSD/travel-optimization.md` covers shortening the travel between paths
by ordering them. This covers routing a travel that is already fixed, and the
one condition that switches that routing off.

## The region

With `reduce_crossing_wall` on, the G-code export plans a travel inside a
region built for the layer it travels on
(`AvoidCrossingPerimeters::init_layer`, `get_boundary`): the layer's wall
contours eroded by one extrusion spacing, plus the holes that region encloses
shrunk by the same spacing, which leaves an unwritten band along every
perimeter; plus a band along the outermost perimeter; with the top surfaces
taken out.

The planner replaces a move that enters and leaves the same region polygon — a
chord over material — by a path along that polygon's border. A move whose end
points lie in different polygons stays straight, because no border connects
them, and a move whose detour is longer than `max_travel_detour_distance` stays
straight as well, so a detour never costs more than the crossing it prevents.
The budget is compared in the scaled units of the region, which is what the
coordinates of a planned path are measured in.

The region is per layer, built when the layer starts. A travel at a layer
change is planned against the layer it travels on, which carries the material
the nozzle passes over.

## When planning is off

A planned path is only valid if the toolhead starts where the slice thinks it
does, and a path is only worth planning if the layer has material to keep off:
planning is skipped for a layer without a `Layer` of its own (a brim apron),
for a travel already marked by `disable_once`, and for a travel whose start
position the writer does not trust.

Custom G-code is the only thing the slice cannot see through, so every
insertion point that can move the toolhead — machine start, tool change,
wrapping detection, timelapse — marks the writer position as unclear, and the
travel that follows is a straight line.

The marker is set on the block that reaches the output, after placeholder
substitution: a timelapse block that collapses to comments and machine markers
leaves the position clear, so the first travel of the layer is planned like any
other, and a block that commands a move — a photo position, a purge wipe, a
park — clears it. A block that moves in Z alone counts as moving the toolhead,
because only Z is tracked through custom G-code.

Where a move by the machine start block is expected, the slice parses the
position its block ends at (`GCodeProcessor::get_last_position_from_gcode`,
`m_pending_start_gcode_position`) and takes the start point of the first travel
from it, so the move is measured from where the toolhead really is. Only the
machine start block feeds that position.

## Lifts

A lift is a Z move, not a travel. It is issued lazily: a retraction records the
point, and the first travel that needs the nozzle clear of the print executes
the lift — vertically, as a spiral, sloped, or as a straight diagonal, per
`z_hop_types`. Because that travel consumes the pending retraction, it is not
eligible for a lift of its own.
