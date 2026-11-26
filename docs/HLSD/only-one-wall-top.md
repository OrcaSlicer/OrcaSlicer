# Only one wall on top surfaces — High Level Design

## Purpose and scope

`only_one_wall_top` leaves a single wall, the outer one, where a layer has a top surface, so the top fill runs up
to the outer wall instead of stopping at a band of inner walls. The topmost layer of an object, which is all top
surface, is printed with one wall. Below it, the perimeter generators find the top surface of each island and deal
with the inner walls that run over it.

The option needs a top shell to act on. With `top_shell_layers` at 0 the top surfaces are retyped as internal (see
`LayerRegion::prepare_fill_surfaces()`), so `has_top_shell_layers()` switches the option off and the GUI hides it.
`only_one_wall_first_layer` is its bottom counterpart: it thins the first layer to one wall, and needs bottom shell
layers in the same way.

A top surface is the part of an island that the next layer leaves uncovered. Bridges keep their walls, and a top
surface narrower than `min_width_top_surface` next to the geometry above is not treated as top, so thin features
such as lettering on the next layer do not strip the walls under them.

## Two strategies

What happens to the inner walls depends on whether the top fill can take their place
(`top_fill_replaces_inner_walls()`).

- **Rerouting.** With `top_surface_expansion` at 0 or `top_surface_density` at 0%, the inner walls are generated
  on the part of the island that is not top surface only. They stop at the top surface and run along its edge, so
  every feature rising from a top surface is ringed by inner walls. Projects that do not use the expansion get
  this result.
- **Removal.** With both set, the walls are generated on the real geometry, as with the option off, and the inner
  walls that run over the top surface are then removed. `detect_surfaces_type()` grows the top surface by
  `top_surface_expansion` under the features rising from it, inside the island less one wall, so the top fill runs
  on under those features instead of being cut up by the walls that rerouting would put around them.

Crossing zero with either setting switches between the strategies, so it regenerates the walls; other changes to
them only redo the fill steps.

## Which walls run over the top

The walls are generated before the surfaces are typed, so the perimeter generator works from the real top surface,
never from the expanded one.

A wall runs over the top by the length of its centerline that lies inside the top region (`over_top_length()`).
Testing its vertices is not enough: a straight wall along a ledge crosses the top between two vertices that both
sit under the geometry rising at either end of the ledge. A wall whose bounding box misses the region is skipped
without any clipping.

The classic generator's region is the area the top fill takes (`top_fill_area()`): the top fills grown by half the
outer wall spacing, to meet the outer wall, which is exactly what becomes the top fill later. The not-top
complement that `split_top_surfaces()` returns does not do: when gap fill is on it counts as not top the band
within one outer wall spacing of the outer wall, which the rerouting strategy leaves to gap fill. With the precise
outer wall off, or a wall order other than inner/outer, and equal wall spacings, the first inner wall lies exactly
on the inner edge of that band. Measured against the complement it never runs over the top, while the top fill
covers it along a whole exposed edge.

## Classic walls

A classic wall is a closed loop, printed as an `ExtrusionLoop` with its seam, place in the wall order, overhang
detection and fuzzy skin, so it is kept or dropped whole:

- a wall with nothing over the top is kept;
- a wall that only grazes the top, by less than two wall widths, is kept whole, and the band it covers is withheld
  from the top fill so the fill is routed around it;
- any other wall is dropped.

The band a dropped wall covered outside the top region goes to the fill surfaces, so nothing is left void, and the
gaps inside the region are removed from the gap fill. Those fill surfaces are typed later like any other: top fill
where the expansion reaches, internal solid infill with Ensure vertical shell thickness at All, sparse infill with
the other settings.

A dropped loop loses its parts outside the top surface as well. When a loop runs both over a ledge and under the
geometry rising from it, the walls under that geometry go for that layer, and a small top patch crossed by long
loops drops them all.

## Arachne walls

Arachne walls can be open lines, so `clip_inner_walls_over_top()` cuts them instead of dropping them, and the walls
under geometry that keeps rising stay. Each line is measured the same way against Arachne's top region. A line with
nothing over the top is kept and one wholly over it is dropped. Any other line is clipped against the region grown
by half a wall width, because the rounded end of a bead reaches half a width past its endpoint. Clipper treats the
line as open, so it also cuts a closed loop at its start vertex and may reverse pieces; pieces sharing an endpoint
are stitched back together, and pieces shorter than a wall width are dropped. A clip that would remove less than
two wall widths, about the pull-back of one crossing, keeps the line whole and withholds its footprint from the top
fill instead.

The inner walls are numbered after the single outer wall. Arachne's single-wall pass may widen two beads to fill a
wall narrower than three outer walls, which leaves no room for the inner walls outside the top surface. When its
outer walls run more than a tenth of an outer wall width away from the full pass's, for more than a wall width
outside the top surface, the full pass's outer walls and the area inside them are used instead.

## Constraints

- A kept inner wall is never under the top fill; a wall that grazes the top has the fill routed around it.
- With the option off nothing changes, and without a top fill to take their place the inner walls are rerouted.
- Classic walls are dropped whole, Arachne walls are cut. Cutting classic walls would need open inner walls through
  the loop pipeline, with a start and an end at every cut.
- The cost is one clip per inner wall whose bounding box meets the top region, on islands with a top surface below
  the topmost layer.
