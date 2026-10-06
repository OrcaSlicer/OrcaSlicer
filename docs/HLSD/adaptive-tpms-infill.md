# Adaptive TPMS infill — High Level Design

## Purpose and scope

`tpms_adaptive` grades the sparse infill of the Gyroid, TPMS-D and TPMS-FK
patterns inside the object: the cells grow continuously from the surface
towards the center of the object. In `3d` the grading follows the whole shape,
including the top and bottom; in `normal_x`, `normal_y` and `normal_z`
it follows each section of the object normal to that axis, so it does not
change along the axis, as suits a profile extruded along it.
`sparse_infill_density` is the density at the surface, `tpms_interior_density`
the density at the center, and `tpms_adaptive_gradient` picks how the density
goes from one to the other. Only internal sparse infill is graded; the Gyroid
Z-buckling optimization does not apply to it.

The design has two parts: a radial field built once per object, and a pattern
warped around the center of each lobe of a body so that its cell size follows the field.

## Radial field

`TpmsRadialField` gives every point of an object the center of its lobe and a
radial coordinate: 0 at the center, 1 at the surface along the ray from the
center. `PrintObject::prepare_tpms_radial_field()` builds it in
`bridge_over_infill()`, next to the adaptive cubic octree, because the anchoring
infill generated there has to match the printed infill. A field is built for
every mode a region uses, and is shared by the regions using that mode: the
field depends on the geometry only, the densities are applied per region in the
fill.

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
  Where the depth ties along a line or a plane, as in a tall box, the lobe's
  center is the node nearest to the middle of the tied nodes, so the center is
  in the middle of the height and not a column.
- A point belongs to the lobe it is nearest to relative to their depths, so the
  side between two lobes is nearer to the smaller one. Near that side, within a
  tenth of that relative distance, the patterns of both lobes morph into each
  other, so the lines stay continuous.
- The reach of a lobe is the distance from its center to the first exit along
  24 x 48 latitude-longitude directions, smoothed twice over neighbouring
  directions in log space. Towards a neighbouring lobe it stops at twice the
  distance to the side between them, so that side is graded half way, as deep
  as a neck is, rather than as sparse as the center or as dense as the surface.
  The radial coordinate of a point is its distance to the center over the reach
  in its direction. Behind a gap, as across the hole
  of a ring, the radial coordinate is above 1 and the infill keeps the surface
  density.
- Every outside node belongs to its nearest body, so points near a surface find
  their body without a search.

In the 2D modes, every plane of nodes normal to the axis is a field of its own:
the distance transform skips the axis, bodies, lobes and the nearest body are
found within the plane, and the reach is sampled on a circle of 48 directions.
A point is looked up in the two planes around it, the weights of their lobes
interpolated along the axis, so the grading does not step between planes; a
plane without a body uses the nearest one that has one.

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
near two thirds of the surface one. Blending lattices of different densities
changes the topology and follows a distance exactly, but mixes two lattices
wherever it blends, which distorts the pattern, and leaves the core with the few
lines of the sparsest lattice. Filling bands of equal distance with the regular
pattern at their density keeps it intact, but cuts its lines at every band, and
the bands are narrower than the sparse cells.

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

## Constraints

- With `tpms_adaptive` disabled, or for other patterns, the fill parameters are reset
  to their defaults, so they neither change the infill nor split fill batches.
- `Layer::get_sparse_infill_max_void_area()` uses the sparser of the two
  densities, as the voids at the center are that large.
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
