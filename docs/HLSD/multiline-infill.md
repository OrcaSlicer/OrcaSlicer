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
(`FillRectilinear::fill_surface_trapezoidal()`), and so do Adaptive Cubic and
Support Cubic (`FillAdaptive`); the other patterns outline their usual
centerlines.

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

## Adaptive Cubic

Adaptive Cubic and Support Cubic take their lines from an octree of cubes
standing on a corner. On each layer every cube cuts its three mid-planes into
segments of the same three 60° families as Cubic, but the pattern is not
periodic. Smaller cubes near the surface add finer lines, and a finer line ends
where it meets the wall of its coarser cube, so the lines form crossings and
T-junctions. `FillAdaptive::multiline_paths()` builds the paths from these
segments directly, for each fill region and within `4 * d1` of it.

At a crossing the two paths bounce as in Cubic. At a T-junction the through line
runs straight on and the path of the ending line stops there. Every path still
runs left to right in the frame where one family is horizontal, and that family
rotates with the layer.

Every line of every cube size lies on one fine lattice, so crossings closer than
a few `d1` are the corners of one small triangle of that lattice, as in Cubic.
The cuts follow the Cubic rules without a closed formula:

- The two bends of a crossing are cut `d1` apart, `d1 / 2` each, perpendicular
  to their bisector, so their walls touch. A cut goes no further than the path
  end, and the other bend takes the rest of `d1`.
- At the tip of a small triangle, between the two slanted families, a cut also
  goes no further than the neighbouring bend turning the other way, and the
  path beyond that bend is kept a wall away from it. The bends onto the
  horizontal family are not limited this way: pushing their paths apart would
  open gaps between walls that should touch.
- A cut moves the path only where the cut line lies beyond it, near its bend.
  The sharp bends between the two slanted families are cut after the bends onto
  the horizontal family, so the tip of a small triangle wins, as in Cubic.
- A path stopping at a T-junction is trimmed until it is `d1` less half a line
  spacing from every other path, so that its end overlaps the wall it stops on
  by half a line and bonds to it. The paths are trimmed one at a time against
  the others as already trimmed, so two ends facing each other meet instead of
  both backing off. A path stopping on the line of another is trimmed before
  that one, so it gives way and the other still reaches the line it stops on. A
  second round trims every path again from its full length, so an end grows
  back where the ends it gave way to were trimmed later, and a last round only
  shortens them, keeping them that far apart. Paths shorter than `d1` are left
  out.
- A line that ends on another less than `2 * d1` past a crossing stops at that
  crossing instead, the shorter one where both do. The path along such a stub
  would be trimmed away, leaving a hole between the walls that were cut to
  touch it.

Short paths enclosed by coarser lines still print as closed outlines, but most
paths run on across several cells.
