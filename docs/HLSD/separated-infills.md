# Separated infills — High Level Design

## Purpose and scope

An object's infill patterns are laid out from one reference point, the center
of the object. When an object groups several parts that do not touch, every
part cuts the same object-wide pattern at a different place, so equal parts get
different infill. `separated_infills` lays the infill of every connected body
out from the center of that body instead, as if the body were sliced on its own.

The option covers sparse infill, internal solid infill and bridges. Top and
bottom surfaces are left to `center_of_surface_pattern`, which centers the
Archimedean Chords and Octagram Spiral surface patterns. The option is off by
default; with it off, or for an object made of a single body, no fill changes.
Adaptive Cubic and Support Cubic do not depend on the option: they always fill
each body on its own (see Octree infill).

## Bodies

`PrintObject::prepare_infill()` groups the islands of every layer (`lslices`)
into 3D connected bodies before bridges are detected, so bridge anchors and
printed infill share one origin. Islands on adjacent layers belong to one body
when their slices overlap. Parts that touch or overlap form one body. Separate
parts, disconnected islands of one mesh, and interleaved parts that never touch,
such as chain links, each form their own. Every island stores the bounding box
of its whole body in `Layer::lslices_separated_component_bboxes`, and the index
of that body in `Layer::lslices_separated_component_ids`.

The pass runs when a region uses separated infills, per-model surface centering
or an octree infill pattern. It is skipped when the object has one model part
that cannot be split, since a single body already shares the object center.

## Centering a fill

`infill_body_island()` matches each fill region to the island it overlaps most,
and `infill_bounding_box()` moves the bounding box of the filler onto the
center of that island's body. The box keeps the extent of the whole object, so
coverage and cost do not change; only its center moves. Bridge anchoring
(`Layer::generate_sparse_infill_polylines_for_anchoring()`) makes the same
choice, so the anchors match the printed infill.

The patterns follow the moved center in one of two ways:

- Rectilinear and its variants, Line, Grid, Triangles, Tri-hexagon, Cubic,
  Quarter Cubic, Lateral Lattice, Lateral Honeycomb and the plane-path patterns
  (Hilbert Curve, Archimedean Chords, Octagram Spiral) phase their layout
  through the box center.
- Honeycomb, 3D Honeycomb, Cross Hatch, Gyroid, TPMS-D and TPMS-FK are laid out
  from the coordinate origin, which is the object center. They return true from
  `Fill::aligned_to_origin()`, and `Fill::fill_surface()` moves each region so
  that the box center lands on the origin, fills it, and moves the paths back.
  With the default box the center is the origin, so nothing moves.

`is_separable_infill_pattern()` lists these patterns. The settings show the
option only when the sparse infill pattern is one of them.

## Octree infill

Adaptive Cubic and Support Cubic take their lines from an octree, laid out from
the center of the mesh it is built from and refined near its surfaces. An
octree of the whole object would lay every part out from the object's center
and refine it near the other parts, so these patterns
(`is_octree_infill_pattern()`) always fill each body on its own, and the
settings hide the option for them.

Besides the octree of the object, `PrintObject::prepare_adaptive_infill_data()`
builds one per body (`FillAdaptive::Octrees`) from the triangles of that body
only, which is the octree the body gets when sliced alone. Each connected
component of the mesh goes to the body that most of a few sampled triangles lie
on. A sample is taken a layer height inside the solid, behind the triangle, and
looked up in the islands of the nearest layer. Each internal bridge surface goes
to the body of its island. The fill takes the octree of the region's body, from
the same `infill_body_island()`; a body that received no triangles uses the
object's octree.

## Patterns left out

Lightning grows its trees over the whole object, so moving a reference point
cannot center it on one body. Concentric and Spiral Inset follow the outline of
each region and need no centering.

Solid infill at full density spaces its lines over the extent of each region,
so it is already independent of the other bodies. Only bridges, which keep
their line spacing, and the plane-path solid patterns depend on the center.
