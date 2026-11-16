# Infill corner smoothing — High Level Design

## Purpose and scope

`sparse_infill_smooth_factor` rounds the corners of the sparse infill paths of
the patterns `is_smoothable_infill_pattern()` lists, so the toolhead does not
slow to near a stop in every corner. `FillCornerSmoothing` does the rounding for
every pattern; the fills only call it on their paths before clipping and before
`multiline_fill()` copies them.

The design is shaped by how firmware plans speed. Klipper, Marlin 2 with
junction deviation and OrcaSlicer's estimator limit the speed at the junction of
two moves by how much the path turns there, and by the length of the moves for
the centripetal part. A curve is printed as straight moves, so its speed depends
on how it is cut into moves as much as on its shape, and the cut decides the size
of the G-code.

## The curve

Each corner is replaced by a curve that runs from `reach` before it along the
incoming leg to `reach` after it along the outgoing one, `reach` being the factor
times half of the shorter leg, so the curves of two corners never overlap.

The curve is a quintic Bézier curve whose first three control points on each leg
are equally spaced, 0.3 of the reach apart, which makes its curvature zero at the
legs: the toolhead eases into the turn. A circular arc takes fewer vertices within
the resolution and prints slower, and a septic curve, whose rate of curvature is
also zero at the legs, prints no faster than the quintic one and takes longer to
slice. Tuning the spacing to the turn angle lowers the peak curvature of sharp
turns but made no measurable difference to print time.

## The vertices

A corner is printed as the polygon of the curve's tangents taken at equal steps
of direction: every vertex turns by the same angle, which spends each vertex
where the planner needs it. Uniform steps of the curve parameter put vertices
where the curve is nearly straight and leave the sharpest turn in the middle.

The number of vertices follows the G-code resolution (`resolution`), as the rest
of the G-code does: it is the fewest that keep the polygon within the resolution
of its curve. The arc of the curve's peak curvature gives the first count, which
the flatter parts of the curve may raise. The count is never so high that a
vertex stands out of the chord of its neighbours by less than the resolution, as
`LayerRegion::simplify_*` simplifies the infill to it afterwards and would drop
that vertex again.

The vertices depend on the geometry and the resolution only, not on the printer.
A finer resolution splits a corner into more and gentler turns, which a planner
takes faster, for a larger G-code file.

A corner of a given turn and vertex count is the same shape at any reach and
resolution, scaled. Each thread builds a shape once at unit reach and all its
`CornerSmoother`s share it, as some fills start a smoother for every path, every
layer starts its own, some patterns (Octagram Spiral) change the reach at every
corner, and searching the tangents of the curve is what slicing spends its time
on.

## Constraints

- At 100% every corner gets the largest curve its legs allow: the factor sets
  the shape of the corners, not their speed. A large curve split into few
  vertices can print slower than the sharp corner, as every vertex is a slowdown
  and the short moves between them leave no room to accelerate.
- With the factor at 0 the paths are unchanged, and the fills keep their order of
  operations, so that rounding to the integer grid does not move a point.
- The plane-path fills (Hilbert Curve, Octagram Spiral) produce their path point
  by point in units of the line distance, so `CornerSmoother` takes points one
  at a time, with the resolution in the same units.
