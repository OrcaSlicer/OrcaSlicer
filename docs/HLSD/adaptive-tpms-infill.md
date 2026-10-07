# Adaptive TPMS infill — High Level Design

## Purpose and scope

`tpms_adaptive` grades the sparse infill of the Gyroid, TPMS-D and TPMS-FK
patterns inside the object: the cells grow continuously from the surface
towards the center of the object. `distance_warp`, `smooth_blend` and
`stepped_shells` follow the distance to the nearest surface, including the top
and bottom, like concentric shells; `lobes` follows the whole 3D shape towards
the center of each lobe of the object; `normal_z`, `normal_y` and `normal_x`
follow each section of the object normal to that axis, so the grading does not
change along the axis, as suits a profile extruded along it.
`sparse_infill_density` is the density at the surface, `tpms_interior_density`
the density at the center, and `tpms_adaptive_gradient` picks how the density
goes from one to the other. Only internal sparse infill is graded; the Gyroid
Z-buckling optimization does not apply to it.

The design has two parts: a field built once per object, and a pattern made
from it, warped around the center of each lobe of a body so that its cell size
follows the field, or, in the modes following the distance to the surface,
split into shells or blended between densities.

## Radial field

`TpmsRadialField` gives every point of an object the center of its lobe and a
radial coordinate: 0 at the center, 1 at the surface along the ray from the
center. `PrintObject::prepare_tpms_radial_fields()` builds it in
`bridge_over_infill()`, next to the adaptive cubic octree, because the anchoring
infill generated there has to match the printed infill. A field is built for
every mode a region uses, and is shared by the regions using that mode: the
field depends on the geometry only, the densities are applied per region in the
fill. An object thinner than the grid cells has no body in the field; no field
is kept then, and the infill falls back to the regular pattern. In the modes
following the distance to the surface, the field also gives the depth of every
point (see below).

A regular 3D grid of cubic cells is rasterized from the `lslices` of the layers,
so the field follows what is printed: negative volumes, the union of
overlapping parts and holes are taken into account, and the mesh does not need
to be closed. A padding node around the grid is always outside. The grid is
capped at about a million nodes, with cells no smaller than 0.5 mm.

- Bodies are the connected inside nodes. Each is graded on its own, so separate
  parts of one object each get their own sparse center.
- A body is split into lobes around the local maxima of the depth, by an exact
  Euclidean distance transform (Felzenszwalb and Huttenlocher, one pass per
  axis). Two maxima are in separate lobes when the depth along the segment
  between them drops below 0.8 of the shallower one, like at the neck between
  two united spheres; maxima shallower than 0.3 of the deepest one are ignored.
  A maximum joins the first lobe whose first maximum it sees without a neck.
  The lobes are made one at a time, the remaining maxima tested against the
  first one in parallel, as a plate has a whole plane of them.
  Where the depth ties along a line or a plane, as in a tall box, the lobe's
  center is the node nearest to the middle of the tied nodes, so the center is
  in the middle of the height and not a column.
- A point belongs to the lobe it is nearest to relative to their depths, so the
  side between two lobes is nearer to the smaller one. Near that side, within a
  tenth of that relative distance, the patterns of the lobes morph into each
  other, so the lines stay continuous. Every lobe in that range takes part, up
  to four, so the morph is also continuous where three or four lobes meet.
- The reach of a lobe is the distance from its center to the first exit along
  24 x 48 latitude-longitude directions, smoothed twice over neighbouring
  directions in log space. Towards a neighbouring lobe it stops at twice the
  distance to the side between them, so that side is graded half way, as deep
  as a neck is, rather than as sparse as the center or as dense as the surface.
  Only the lobes whose centers are near enough to be nearer at the current
  distance are compared along a ray, so many lobes, as in a perforated plate,
  stay cheap.
  The radial coordinate of a point is its distance to the center over the reach
  in its direction. Behind a gap, as across the hole
  of a ring, the radial coordinate is above 1 and the infill keeps the surface
  density.
- Every outside node belongs to its nearest body, so points near a surface find
  their body without a search. With a single body, all nodes belong to it.

In the 2D modes, every plane of nodes normal to the axis is a field of its own:
the distance transform skips the axis, bodies, lobes and the nearest body are
found within the plane, and the reach is sampled on a circle of 48 directions.
A point is looked up in the two planes around it, the weights of their lobes
interpolated along the axis, so the grading does not step between planes; a
plane without a body uses the nearest one that has one. The planes are a cell
apart, not a layer: where the sections change abruptly, as at a step, the
patterns of the two planes morph into each other over that cell.

A distance to the nearest surface would be the obvious field, but no smooth map
follows it. By the divergence theorem, the mean scale of a map over a body is
fixed by its values on the surface: a map that keeps the full density along the
whole surface, as the distance would ask under the top and bottom, has the mean
density of the uniform infill, the sparser core being paid for by lines crowding
along the walls. The layers of a plate at different depths would also need
different line spacings in the same directions, which no continuous map allows
without shearing across the plate. Following the distance needs changes of the
topology of the lattice (see below). The radial coordinate instead grades what a
single map can: towards one point.

## Warped pattern

The pattern is evaluated on warped coordinates:

    TPMS(f_surface * m(t) * (p - center))

where `m` scales the pattern around the center of the lobe: its frequency is
`m + t * m'` along the ray and `m` across it. `m(t)` is the mean of the target
scale over the ball of radius `t`, `3 / t^3 * integral of s^2 * target(s) ds`, so
the mean of the three, and with it the density, follows the gradient. The cells
are round at the center; near the surface they are flattened, with the lines
running parallel to it. Beyond the surface the target is the surface scale, so
the warp extends continuously outside.

In the 2D modes only the coordinates within the plane are warped, and `m(t)` is
the mean over the disc, `2 / t^2 * integral of s * target(s) ds`. Along the axis
the pattern keeps the interior frequency: scaling it with `m` would shear the
pattern by the distance along the axis times the gradient of `m`, without bound
on a long object. The cells are round at the center and stretched along the
axis near the surface. With Normal Z the layers are graded exactly, since
the lines of a layer follow its in-plane frequencies; normal to X or Y, the
layers near the sides are as dense as the larger of the two frequencies in the
layer, which is the surface one.

Evaluating a TPMS at a frequency that varies with the position without such a
map distorts it wherever the frequency changes, because the phase also changes
with the gradient of the frequency times the distance from the origin. Fitting a
smooth map to a varying isotropic scale in the least-squares sense (a Poisson
problem per axis) cannot grade strongly: its divergence is the target scale plus
a harmonic function pinned by the surface, which keeps the scale in the core
near two thirds of the surface one. Following a distance exactly needs the
lattice to change its topology, by blending lattices of different densities or
filling shells of equal distance with them, as the modes following the distance
to the surface do.

The target scale at depth `d = 1 - t`, with `S` the surface and `I` the interior
frequency, both from each pattern's own density calibration:

| Gradient    | Scale                  |
|-------------|------------------------|
| Linear      | `1 + (I / S - 1) * d`  |
| Quadratic   | `1 + (I / S - 1) * d^2`|
| Exponential | `(I / S)^d`            |

With a denser surface, quadratic keeps the surface density deepest and
exponential drops fastest. A denser interior works the same way.

The zero level is extracted with marching squares like the regular TPMS-FK, on
a sampling grid fixed in the fill frame like the optimized Gyroid, so that every
region of a layer connects its lines the same way at the saddles of the pattern.
Loops narrower than two lines (shorter than `2 * PI * spacing`) are dropped, as
they would print as blobs. The fill works in a frame rotated by the infill
angle, so the radial field is looked up at the point rotated back into the
object frame, and the center rotated into the fill frame. Both use the middle of
the layer.

## Modes following the distance to the surface

`distance_warp`, `smooth_blend` and `stepped_shells` grade by the distance to
the nearest surface, including the top and bottom, as concentric shells do. The
depth of a point is that distance over the distance of the deepest point of its
body, from 0 at the surface to 1; the field keeps it for every node and
interpolates it between them. A tall box so keeps its whole axis as sparse as
its center, and a plate is graded through its thickness.

No single smooth pattern follows that depth without distortion (see above), so
the three modes trade differently:

- Distance warp keeps the lobes and the warp of Lobes, but its radial profile
  comes from the depth. For every direction of a lobe, the mean depth over the
  ball along the ray is sampled at 33 radii up to the reach, then smoothed over
  the neighbouring directions like the reach. The radial coordinate is the one
  of the linear profile with the same mean depth, `t = 4/3 * (1 - mean depth)`,
  so with a linear gradient the mean cell size follows the depth exactly, and
  with the others approximately. In a sphere or a cube, where the depth falls
  linearly along every ray, it is Lobes. Elsewhere the profile changes with the
  direction, and the warp shears where neighbouring directions differ, as in
  plates and long bodies; right under the top of a long body the cells are
  sparser within the layer, the warp moving their density into the height.
  The profiles are smoothed over the directions like the reach, as sharper
  ones shear the pattern across the layer, which adds lines. A long body is so
  graded partly along its length, between Lobes and the distance.
- Smooth blend evaluates the regular patterns of the two levels around the
  target of every point and blends them by a smoothstep over the whole gap
  between the levels, here at most 2.5 times apart. The density follows the
  depth without steps, but where two lattices blend, part of their lines run
  along the blend, so fewer levels print fewer extra lines. From 25% to 5%,
  three levels print about 0.45 of the uniform infill in a deep core whose
  levels alone would print 0.3; levels 1.5 times apart print 0.6 to 0.7, and a
  single blend from the surface to the interior 0.6.
- Stepped shells split each region of a layer into shells and fill every shell
  with the regular pattern at its density. The densities are levels from the
  surface to the interior density at most 1.5 times apart, five from 25% to 5%,
  and a point takes the level nearest to its target on that geometric scale.
  The shells are traced by marching squares of the continuous level over the
  layer on a 0.5 mm grid fixed in the object, so every region of a layer gets
  the same shells, and clipped to the region. Each shell is shrunk by half a
  line, like a filled region, and its regular filler connects its lines along
  that boundary, so the connections of two neighbouring shells lie side by side
  instead of on top of each other. The pattern is never
  distorted, but its lines end at every shell, and thin parts get thin shells.
  The connections add lines: in the core of a 60 mm cube, about a third more
  than the target.

Stepped shells and Smooth blend need neither lobes nor reaches, which are not
built for them.

## Constraints

- With `tpms_adaptive` disabled, or for other patterns, the fill parameters are reset
  to their defaults, so they neither change the infill nor split fill batches.
- `Layer::get_sparse_infill_max_void_area()` uses the sparser of the two
  densities, as the voids at the center are that large.
- At a sparse infill density of 100% the sparse infill is turned into solid
  infill, so there is nothing to grade: the options are hidden and no field is
  built.
- The adaptive options invalidate `posPrepareInfill`, which rebuilds the field
  and the anchoring infill.
- An elongated body without a neck has one center, so its far ends are graded
  as the outer part of the body, and the warp shears where the reach changes
  quickly with the direction. A concave body, like an L, may be split into lobes
  where its maxima cannot see each other in a straight line.
- Across a ray, the scale is the mean of the gradient from the center, so the
  layers right under the top and above the bottom are sparser than the surface
  density in their middle, and a plate is graded from its middle outwards rather
  than through its thickness.
