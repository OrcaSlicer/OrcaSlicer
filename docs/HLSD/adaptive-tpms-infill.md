# Adaptive TPMS infill — High Level Design

## Purpose and scope

`tpms_adaptive` grades the sparse infill of the TPMS-D and TPMS-FK patterns with
the depth inside the object. `sparse_infill_density` is the density at the
surface, including the top and bottom, and `tpms_interior_density` the density
at the deepest point of the object. `tpms_adaptive_gradient` picks how the
density goes from one to the other. Only internal sparse infill is graded.

The design has two parts: a depth field built once per object, and a pattern
whose local density follows the field.

## Depth field

`TpmsDepthField` samples the depth on a regular 3D grid of cubic cells over the
object. `PrintObject::prepare_tpms_depth_field()` builds it in
`bridge_over_infill()`, next to the adaptive cubic octree, because the anchoring
infill generated there has to match the printed infill. It is built only when a
region uses the feature, and is shared by all regions: the field depends on the
geometry only, the densities are applied per region in the fill.

Each grid level is rasterized from the `lslices` of the layer at that height,
so the depth follows what is printed: negative volumes, the union of
overlapping parts and holes are taken into account, and the mesh does not need
to be closed. A padding node around the grid is always outside. An exact
Euclidean distance transform (Felzenszwalb and Huttenlocher, one pass per axis)
gives the distance of every inside node to the nearest outside node, less half
a cell to estimate the distance to the surface. The distances are normalized by
their maximum, so the depth is 0 at the surface and 1 at the deepest point of
the object, and the gradient needs no length parameter. Queries interpolate
trilinearly.

The grid is capped at about a million nodes, with cells no smaller than 0.5 mm.
The grading changes over the depth of the object, so the cell size is small
next to it.

## Graded pattern

Scaling the frequency of a TPMS with the position distorts it, as the phase
also changes with the gradient of the frequency. Instead, the field is a blend
of two lattices at fixed frequencies, the one of the denser and the one of the
sparser density:

    F = w * TPMS(f_dense * p) + (1 - w) * TPMS(f_sparse * p)

and the infill is its zero level, extracted with marching squares like the
regular TPMS-FK. The lines stay continuous across the transition. Where the two
lattices meet, the blend pinches off small closed loops; those narrower than two
lines (shorter than `2 * PI * spacing`) are dropped, as they would print as
blobs.

The density of the blend is not linear in `w`: the dense lattice persists
until `w` falls below about one half, then gives way quickly. The normalized
density as a function of `w` was measured on both patterns at several density
pairs (0.20/0.05, 0.25/0.08 and 0.30/0.10); it depends little on the pattern and
the ratio. Its inverse is tabulated in `dense_weight()`, so a target density
maps to the weight that produces it. Measured on sliced slabs, the density
follows the three gradients within the noise of the pattern cells.

The target density at depth `t`, with `S` the surface and `I` the interior
density:

| Gradient    | Density               |
|-------------|-----------------------|
| Linear      | `S + (I - S) * t`     |
| Quadratic   | `S + (I - S) * t^2`   |
| Exponential | `S * (I / S)^t`       |

With a denser surface, quadratic keeps the surface density deepest and
exponential drops fastest. A denser interior works the same way, with the roles
of the two lattices swapped.

The frequencies come from each pattern's own density calibration, so a
constant depth gives the regular pattern's density. The fill works in a frame
rotated by the infill angle, so the depth is looked up at the point rotated
back into the object frame, at the middle of the layer.

## Constraints

- With `tpms_adaptive` off, or for other patterns, the fill parameters are reset
  to their defaults, so they neither change the infill nor split fill batches.
- `Layer::get_sparse_infill_max_void_area()` uses the sparser of the two
  densities, as the interior voids are that large.
- The adaptive options invalidate `posPrepareInfill`, which rebuilds the field
  and the anchoring infill.
