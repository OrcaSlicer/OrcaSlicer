# Multiline infill — High Level Design

## Purpose and scope

`fill_multiline` prints every sparse infill wall as N adjacent lines instead of
one, so a wall is `d1 = N * spacing` thick. Only internal sparse infill uses it.
Each pattern first builds its single-line centerlines at N times the usual line
spacing (so the density holds), and `multiline_fill()` then replaces each
centerline by the lines of that wall: the centerline itself when N is odd, and
closed outlines around it at every `spacing` out to `d1 / 2`. The outlines are
clipped to the fill region contracted by half a line width, then connected like
any other infill.

Outlines of centerlines that cross each other overlap at every crossing, which
over-extrudes the wall intersections. The line-crossing patterns Grid,
Triangles, Tri-hexagon and Cubic therefore build centerlines that never cross
(`FillRectilinear::fill_surface_trapezoidal()`); the other patterns outline
their usual centerlines.

## Non-crossing centerlines

The crossing lines are resolved into x-monotone paths, the levels of the line
arrangement: walking along x, the k-th path is always the k-th line from the
bottom. At every crossing, the two paths bounce off each other instead of
passing through. Adjacent paths meet only at crossings, so their outlines touch
there and nowhere overlap.

Where two paths meet, each is cut short by a line perpendicular to the bisector
of its bend, `d1 / 2` from the crossing. The two cut segments are parallel and
`d1` apart, so the outermost lines of the two walls sit exactly `spacing` apart,
like the lines inside a wall. Where three lines meet at one point, the middle
path runs straight through and the outer two are cut `d1` from it.

Each pattern builds its rows along x in a rotated frame. Grid lines run at ±45°
there, and its rows are trapezoid waves that transpose on alternate layers. The three families of Triangles, Tri-hexagon and
Cubic run at 0°, 60° and 120°. Those rows rotate by 120° every layer about a
3-fold center of the arrangement, so each family takes every role in turn.
The pattern is phased on fixed positions, so it lines up across layers and
across the regions of one layer. Rounding the corners with
`sparse_infill_smooth_factor` happens before `multiline_fill()`.

## Cubic

Single-line Cubic draws the three families at the same spacing `h` and shifts
them with z: by `+dx`, `-dx` and `+dx`, `dx = z / sqrt(2)`. The multiline paths
follow the same lines. In the frame where one family is horizontal, the other two
cross in rows `h` apart, alternating by half a period, at height
`tau = -3 * dx (mod h)` above the horizontal line below them. The crossings split
every band between horizontal lines into up-pointing triangles of height `tau`,
down-pointing triangles of height `h - tau`, and hexagons. At `tau = 0` (and `h`)
all three families meet at common points, as in Triangles. At `tau = h / 2` the
triangles are equal, as in Tri-hexagon. The origin of that frame is always a
3-fold center, whatever z is, so the per-layer rotation keeps the lines in place.

Each band holds two paths that touch at its crossings: the upper one takes the
V below the crossing and runs along the top horizontal line, and the lower one
takes the inverted V above it and runs along the bottom line. Both are the same function
of `tau`, the lower one mirrored with `h - tau`. `cubic_upper_level()` builds one
period of the upper path as the lower envelope of five lines, clipped from below:

- the two slanted lines through the crossings,
- the horizontal line, lowered when the triangle above it is less than `1.5 * d1` high,
- the two chamfers where the path turns onto and off the horizontal line, `d1 / 2`
  from those crossings,
- the flat cut into the V at the crossing.

The cut height `clamp(tau - d1 / 2, 0, h - d1) + d1` is what makes the pattern
continuous in z. While both triangles are at least `1.5 * d1` high, every
crossing is a pair of bends `d1 / 2` from it, as in Tri-hexagon. When a triangle
is thinner, its three paths stack like a triple crossing. The path through it
flattens toward its base line and lies on it once the triangle is under `d1 / 2`
high, and the paths beside it are pushed `d1` away. The layout thus reaches the
Triangles one where the families meet. Adjacent paths stay at least `d1` apart
at every `tau` and at every density up to 100%.
