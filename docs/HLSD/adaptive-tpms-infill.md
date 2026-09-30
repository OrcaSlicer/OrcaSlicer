# Adaptive TPMS infill — High Level Design

## Purpose and scope

`tpms_adaptive` grades the sparse infill of the Gyroid, TPMS-D and TPMS-FK
patterns inside the object: the cells grow continuously from the surface,
including the top and bottom, towards the center of the object.
`sparse_infill_density` is the density at the surface, `tpms_interior_density`
the density at the center, and `tpms_adaptive_gradient` picks how the density
goes from one to the other. Only internal sparse infill is graded; the Gyroid
Z-buckling optimization does not apply to it.

The design has two parts: a radial field built once per object, and a pattern
warped around the center of each body so that its cell size follows the field.

## Radial field

`TpmsRadialField` gives every point of an object the center of its body and a
radial coordinate: 0 at the center, 1 at the surface along the ray from the
center. `PrintObject::prepare_tpms_radial_field()` builds it in
`bridge_over_infill()`, next to the adaptive cubic octree, because the anchoring
infill generated there has to match the printed infill. It is built only when a
region uses the feature, and is shared by all regions: the field depends on the
geometry only, the densities are applied per region in the fill.

A regular 3D grid of cubic cells is rasterized from the `lslices` of the layers,
so the field follows what is printed: negative volumes, the union of
overlapping parts and holes are taken into account, and the mesh does not need
to be closed. A padding node around the grid is always outside. The grid is
capped at about a million nodes, with cells no smaller than 0.5 mm.

- Bodies are the connected inside nodes. Each is graded on its own, so separate
  parts of one object each get their own sparse center.
- The center of a body is its deepest node, by an exact Euclidean distance
  transform (Felzenszwalb and Huttenlocher, one pass per axis). Where the depth
  ties along a line or a plane, as in a tall box, the node nearest to the middle
  of the deepest nodes is taken, so the center is in the middle of the height
  and not a column.
- The reach of a body is the distance from its center to the first exit along
  24 x 48 latitude-longitude directions, smoothed twice over neighbouring
  directions in log space. The radial coordinate of a point is its distance to
  the center over the reach in its direction. Behind a gap, as across the hole
  of a ring, the radial coordinate is above 1 and the infill keeps the surface
  density.
- Every outside node belongs to its nearest body, so points near a surface find
  their body without a search.

A distance to the nearest surface would be the obvious field, but it makes a
column of equal depth along the axis of a tall object, and a smooth map can
only grade it weakly (see below).

## Warped pattern

The pattern is evaluated on warped coordinates:

    TPMS(f_surface * m(t) * (p - center))

where `m` scales the pattern around the center of the body: its frequency is
`m + t * m'` along the ray and `m` across it. `m(t)` is the mean of the target
scale over the ball of radius `t`, `3 / t^3 * integral of s^2 * target(s) ds`, so
the mean of the three, and with it the density, follows the gradient. The cells
are round at the center; near the surface they are flattened, with the lines
running parallel to it. Beyond the surface the target is the surface scale, so
the warp extends continuously outside.

Evaluating a TPMS at a frequency that varies with the position without such a
map distorts it wherever the frequency changes, because the phase also changes
with the gradient of the frequency times the distance from the origin. Fitting a
smooth map to a varying isotropic scale in the least-squares sense (a Poisson
problem per axis) cannot grade strongly: its divergence is the target scale plus
a harmonic function pinned by the surface, which keeps the scale in the core
near two thirds of the surface one. Blending a dense and a sparse lattice
grades exactly, but leaves the core with the few lines of the sparse lattice
instead of growing cells.

The target scale at depth `d = 1 - t`, with `S` the surface and `I` the interior
frequency, both from each pattern's own density calibration:

| Gradient    | Scale                  |
|-------------|------------------------|
| Linear      | `1 + (I / S - 1) * d`  |
| Quadratic   | `1 + (I / S - 1) * d^2`|
| Exponential | `(I / S)^d`            |

With a denser surface, quadratic keeps the surface density deepest and
exponential drops fastest. A denser interior works the same way.

The zero level is extracted with marching squares like the regular TPMS-FK.
Loops narrower than two lines (shorter than `2 * PI * spacing`) are dropped, as
they would print as blobs. The fill works in a frame rotated by the infill
angle, so the radial field is looked up at the point rotated back into the
object frame, and the center rotated into the fill frame. Both use the middle of
the layer.

## Constraints

- With `tpms_adaptive` off, or for other patterns, the fill parameters are reset
  to their defaults, so they neither change the infill nor split fill batches.
- `Layer::get_sparse_infill_max_void_area()` uses the sparser of the two
  densities, as the voids at the center are that large.
- The adaptive options invalidate `posPrepareInfill`, which rebuilds the field
  and the anchoring infill.
- Elongated or branched bodies have one center, so their far ends are graded as
  the outer part of the body, and the warp shears where the reach changes
  quickly with the direction.
