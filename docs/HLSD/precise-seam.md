# Precise Seam — High Level Design

## Purpose and scope

Precise Seam lets a helper volume decide where the seam of an object goes. The
user attaches a mesh to an object as a Precise Seam modifier. On every layer,
the part of the external perimeter that lies inside the modifier's slice
determines where the seam must, may or must not be placed. The helper is a
persistent model object rather than paint on the surface, so it keeps working
when the design changes. A body swept along a path on the surface can guide the
seam along any trajectory.

The modifier is non-printing geometry. It takes no part in object slicing,
region assignment, filament selection or brim adhesion, and it affects only seam
placement during G-code export. Objects without Precise Seam volumes follow the
regular seam placement unchanged.

Precise Seam does not replace the seam placer. It feeds it: a modifier inserts
the points it needs into the perimeter and changes the enforced/blocked type of
seam candidates, the same typing mechanism as seam painting, and the configured
seam position then chooses among them.

## Modifier types

Precise Seam adds six `ModelVolumeType` values after `SUPPORT_ENFORCER`, strong
types first and weak types after them. `is_precise_seam()`,
`is_precise_seam_strong()` and `is_precise_seam_weak()` are range checks that
depend on this order.

| Type | Group | Effect on an intersected perimeter |
| --- | --- | --- |
| `PRECISE_SEAM_CENTER` | strong | seam at the midpoint, by arc length, of the intersection |
| `PRECISE_SEAM_LEFT` | strong | seam at the first point of the intersection |
| `PRECISE_SEAM_RIGHT` | strong | seam at the last point of the intersection |
| `PRECISE_SEAM_ENFORCED` | weak | intersection marked as enforced |
| `PRECISE_SEAM_BLOCKED` | weak | intersection marked as blocked |
| `PRECISE_SEAM_NEUTRAL` | weak | intersection reset to neutral |

A strong modifier fixes a single point. The perimeter gets exactly one enforced
seam candidate there, and every other candidate is blocked. A weak modifier
retypes, and where needed adds, the candidates inside its intersection, like
painting does.

An **intersection** is a continuous part of the external perimeter's centerline
whose model surface lies inside the modifier's slice on that layer. It is a
portion of the perimeter, never a chord through the object. The centerline lies
about half an extrusion width inside the printed wall and is invisible to the user,
who places modifiers against the model surface. Extraction therefore
reconstructs that surface from the centerline, clips it, and carries the
result back onto the centerline along the normal (see
[Surface reconstruction](#surface-reconstruction)). A modifier acts when it
reaches the reconstructed surface, which lies a margin of 5 µm inside the model
surface, and has no effect when it stays inside the body, even if it crosses the
centerline. A modifier that enters the model by less than the margin does not
act. An open extrusion (a thin wall printed as one line) is left out: its
G-code does not depend on where the seam is.

### Terms

- **Segment:** an intersection as the code represents it (`PerimeterSegment`).
  User-facing texts call it an intersection.
- **Fragment:** a piece of the perimeter returned by clipping, before it is tied
  to the source contour.
- **Interval:** the bound part of one source edge, given by the edge index and a
  parameter range on that edge.
- **Zone:** a weak segment with its type (Enforced, Blocked or Neutral).
- **Boundary:** an end of a zone, inserted into the perimeter polygon.
- **Candidate:** a seam candidate of the seam placer, built from the points of
  the processed perimeter polygon (painted enforcers may add more).

First and last are taken along the perimeter oriented counter-clockwise as seen
from above. On an outer wall seen from outside, Left is therefore the left end
of the intersection. On the wall of a hole seen from inside the hole, the two
ends are swapped. Mirroring an object does not mirror the mode: perimeters stay
counter-clockwise, so Left remains the left end seen from outside, and the seam
moves to the other end of the modifier instead of following the mirrored model.

## Priority

The order of volumes in the object is the priority order, highest first.
`ModelObject::sort_volumes()` keeps every strong modifier before every weak one
and preserves the user's order within each group. The object list lets the user
drag a modifier only within its own group. A type change that crosses the group
boundary moves the volume to the end of its new group, with the lowest priority
there.

- **Strong:** modifiers are tried in priority order on each perimeter. The first
  one that yields a usable segment decides the seam. Within that modifier the
  longest segment wins; lengths are never compared across modifiers. Once a
  strong point is placed, no later strong modifier and no weak modifier is
  processed for that perimeter.
- **Weak:** every weak modifier applies. They are applied from the lowest
  priority to the highest, so the highest one overwrites overlapping zones.

A strong modifier without a usable segment, even one whose fragments were all
discarded, passes the turn to the next one.

## Data flow

1. **Invalidation.** `Print::apply()` treats a change of Precise Seam volumes as
   a change of seam placement and invalidates G-code export; the object is not
   resliced (see [Print invalidation](#print-invalidation)).
2. **Modifier slices.** `SeamPlacer::init()` collects each object's Precise Seam
   volumes once, slices every volume separately and caches its regions with
   their bounding boxes.
3. **Perimeters.** Seam candidates are gathered in parallel over the layers.
   For objects with Precise Seam volumes, each external perimeter polygon is
   normalized and prepared once for all modifiers.
4. **Extraction.** For each modifier, the perimeter is clipped against the
   modifier's regions on that layer. The clipped fragments are bound back to the
   source edges of the perimeter and assembled into segments.
5. **Strong, then weak.** Strong modifiers try to insert one seam point into the
   perimeter polygon. If none succeeds, weak modifiers insert their zone
   boundaries and subdivide enforced edges.
6. **Candidates.** The seam placer builds candidates from the modified polygon.
   Painting assigns types first, weak zones overwrite them, and a strong point
   makes its candidate the only enforced one.
7. **Selection and restoration.** The configured seam position chooses the
   seams and aligns them. Afterwards the exact strong points are restored.
8. **Warnings.** After all objects are processed, `SeamPlacer::init()` prepares
   one combined warning text if any problem was found; G-code export issues it.

## Modifier slices

`init_precise_seam_data()` collects the Precise Seam volumes of each object:
strong ones in priority order and weak ones in reverse, so that weak zones can
be applied with last-write-wins. Each volume is sliced separately with
`PrintObject::slice_single_volume_regions()`, at the object's layer heights and
with the same centered transformation as the object. The slices keep every
region's outer contour together with its holes as an `ExPolygon`. Volumes are
not merged, so each keeps its own priority, and a modifier may have several
regions on one layer.

`prepare_modifier_slices()` moves the slices into `ModifierRegionsCache`,
pairing each region with the bounding box of its exterior. Empty layers keep
their slots, so the cache is indexed by object layer; `Layer::id()` includes raft
layers, which are subtracted. The cache is filled before candidates are gathered
and is only read afterwards, shared by both modifier kinds and all worker
threads without locking.

## Perimeter preparation

The seam placer works on external perimeter loops, including the walls of
holes. Precise Seam processes closed loops only: an open extrusion (a thin wall
printed as one line) keeps its ordinary seam candidates, and modifiers are not
evaluated on it. A modifier that crosses only such a wall has no effect, and the
"had no effect" warning names it when it was evaluated on a closed loop. For
objects with Precise Seam volumes, consecutive duplicate points and
the repeated closing point of each extrusion loop are removed: adjacent
extrusion paths share endpoints, and the resulting zero-length edges would
prevent point insertion at their junctions. Distinct visits to one point of a
self-touching contour are kept. Objects without Precise Seam volumes keep their
original points, so ordinary seam candidates are unaffected.

Each polygon is made counter-clockwise. A single `PreparedPerimeter` is then
built for all modifiers of that perimeter. It holds a validity check (at least
three points, no consecutive or closing duplicates), the bounding box, the
clipping line (the polygon as an open polyline with its first point repeated at
the end), the side of the air (outside a contour, inside a hole, by the loop
role) and the surface shift. The surface line is built from it on the first
request, so perimeters that no modifier approaches never pay for it.

The **surface shift** is the distance from the centerline to the model surface
the user sees, less a margin. Its nominal value is `d = w/2 - c - m`:

- `w` is the region's nominal external perimeter width, the flow the perimeter
  generator uses for the outer loop on that layer, first layer included.
- `c` is the XY compensation that moved this wall away from the model,
  positive towards air: `xy_contour_compensation` for a contour and minus
  `xy_hole_compensation` for a hole (a positive value enlarges holes). It is
  zero where slicing skips compensation, for multi-material painting with
  several filaments. It is also zero for a hole on a layer with several
  regions when the hole setting is negative: slicing then clips the regions by
  the compensated islands, and clipping cannot add the material that would make
  the hole smaller. Both values are taken once per object in
  `SeamPlacer::init()`, so they describe the outer contours and holes of the
  object. A wall on the boundary between two regions of one object is
  compensated differently by slicing, and there the surface is not restored
  exactly (see [Known limitations](#known-limitations)). Modifier slices are not
  compensated, so they stay in the frame of the model.
- `m` is `SURFACE_MARGIN` (5 µm). A modifier face placed flush against a model
  face would otherwise touch the reconstructed surface by rounding noise and
  act on some layers only; the margin moves that unstable position 5 µm deep,
  where it is not reached by accident.

**Measured shift.** On the layers with elephant foot compensation (printing
directly on the bed, below `elefant_foot_compensation_layers`) and on every
layer of Arachne perimeters (not in spiral vase mode, which uses the classic
generator), the centerline does not lie at depth `w/2`: elephant foot
compensation shrinks the contour the perimeters are built on by an amount that
changes along it, and Arachne lines vary in width. There the shift is measured:
`d(P) = D(P) - c' - m` at a point `P` of the centerline, where `D(P)` is the
depth of `P` below the boundary of the layer's islands (`Layer::lslices`) and
`c'` the part of `c` those islands contain. The depth is the offset from `P` to
the nearest point of the boundary, projected on the normal of the edge (at a
vertex, the larger of the projections on its two edges' normals). At an inner
corner the nearest point is the corner of the boundary, farther away than
either side; its projections are the distances to the sides.

- Slicing stores the islands of an elephant foot layer from before that
  compensation, and those of other layers as the perimeters see them, so `D`
  includes the elephant foot compensation at that point and the half width of
  the line actually printed there. The islands contain the XY compensation
  (`c' = c`), except on an elephant foot layer with several regions: slicing
  keeps those islands from before a negative contour compensation and applies
  it to the perimeters only, so there `c'` is zero for contours.
- A distance is accepted only within the range of the wall's own line: from half
  the narrowest line the generator prints to half the widest, widened by the
  elephant foot compensation of the layer, by the part of `c` missing from the
  islands, by half the difference between line width and spacing (Arachne keeps
  a precise outer wall that much deeper where it prints one line) and by the
  slicing resolution plus 1 µm. The narrowest line is half the nominal width for
  Arachne, which splits a wall into two lines narrower than its minimum bead
  width, and slightly under it for the classic generator, which prints narrow
  loops thinner; the widest is twice the nominal width for Arachne and the
  nominal width for the classic generator. A distance outside that range does not
  come from the wall's own surface: the wall is thinner than a line, or it lies
  on the boundary between two regions, which the islands do not contain. Such a
  point takes the nominal shift.
- Every vertex is measured, and an edge longer than 0.5 mm (the step at which
  elephant foot compensation is resampled) also at points at most 0.5 mm apart
  along it. If they all follow the straight change between its vertices within
  the snapping radius, the edge stays one piece; otherwise they all split it.
- A negative value is taken as zero. A perimeter whose values all equal the
  nominal shift within the snapping radius keeps the constant shift, so a wall at
  its nominal depth gives exactly the result of the nominal shift. A perimeter
  whose largest value is within the snapping radius keeps the centerline; a
  single point at zero does not change that.
- The measurement is decided per loop. A loop with fuzzy skin is measured on its
  smooth centerline (see [Fuzzy skin](#fuzzy-skin)). A noisy line itself is never
  measured: the noise moves it off the smooth outline, so measuring would pull
  its points within the range back to the outline while the points outside it
  kept the noise. A loop without a smooth centerline on a layer where any region
  adds fuzzy skin to the outer walls of that kind of loop keeps the nominal shift,
  since nothing tells an unchanged loop from a noisy one that lost its centerline.
- The islands are indexed once per layer and only on layers where a Precise
  Seam modifier has a region, and only loops that some modifier on the layer
  approaches are measured.

A shift within the snapping radius is treated as zero: extraction then clips the
centerline itself. The preparation borrows the polygon and is used only while the polygon
is unchanged: strong processing returns immediately after inserting its point,
and weak processing collects all segments before it inserts anything. An
invalid perimeter receives no Precise Seam processing.

### Fuzzy skin

Fuzzy skin is applied by the perimeter generator, which resamples a loop and moves
its points off the smooth centerline (Arachne's extrusion mode changes the line
width instead). Only the noisy line reaches the seam placer, and a surface
shifted from it would repeat the noise. The generator therefore keeps the
centerline before texturing of an external loop whose points the texture
changed, in `ExtrusionLoop::untextured` (classic `traverse_loops()`, Arachne
`traverse_extrusions()`, which copies the junctions only where the layer has
fuzzy skin on that kind of loop). A list of points equal to the printed one is
not kept. The field is kept for every such loop, whether or not the object has
Precise Seam volumes, because adding a volume does not regenerate perimeters
(see [Print invalidation](#print-invalidation)). Copying, moving, reversing or
turning the start of the loop keeps it, and so do the simplification and arc
fitting of its paths after the generator, which leave the same textured loop;
giving the loop other geometry must reset it.

For such a loop the preparation has two polygons: the smooth centerline, from
which the surface is reconstructed and on which the shift is measured, clipping,
binding, segments, lengths, Center and full containment are worked out; and the
noisy loop, into which the results are inserted. Both are normalized alike (no
repeated points, counterclockwise).

- **Pairs.** A segment keeps the surface points its ends were carried from, and
  merging intervals keeps the pair of the outer end, also across vertex zero. An
  arc still collapses into its vertex, but the end keeps its real point on the
  arc: a zero-length segment has the two ends of its piece of arc, and separate
  pieces of one arc make one vertex point from the piece nearest the arc's start
  to the end of the farthest. A vertex point that continues a segment gives the
  segment's end its arc point. Left and Right take the pair of their end.
  Center, the middle by length of the smooth segment, takes as its pair the point
  of the shifted edge at the same parameter (shift profile included), the middle
  of the arc or bridge at a vertex (of the piece covered, for a zero-length
  segment), the point where the shifted edges meet at a vertex without either
  (collinear edges; the smooth point itself, as an emergency fallback, should
  the edge have no element left), or the crossing `K` in the dead stretch of a
  concave corner.
- **Transfer.** The line through the surface point and the smooth point is
  intersected with the edges of the noisy loop within a window of the smooth
  point, and the crossing nearest to it is taken; an edge lying along the line
  gives its point nearest to the smooth point. The window is an estimate from the
  settings, not a measured distance or a proven bound. Regions texture a loop one
  after another, each over the result of the previous ones, so it adds up the
  amplitudes of the layer's different noises (regions that differ only in which
  walls they texture make one noise; regions with fuzzy skin disabled make none).
  An amplitude is the thickness times the largest value of its noise module:
  Perlin, Billow and RidgedMulti add octaves and can exceed one. These bounds rest
  on the range libnoise states for its coherent noise, from -1 to 1.
  Arachne's combined mode moves a point by half its change of width, which adds
  half the smallest width it keeps. On top come the largest point distance, the
  farthest a chord cutting a convex corner passes from the smooth point, and a
  micrometre of rounding. There are three outcomes:
  - **exact**: such a crossing exists;
  - **approximate**: the pair coincides (the centerline path, where the surface
    is the centerline) or the line crosses nothing within the window; the
    nearest point of the noisy loop within the window is taken, and counted in
    the log;
  - **failed**: not even that; the point is a discarded fragment, with its
    counter, log marker and the type in the warning. A strong segment whose point
    fails takes no part; a weak zone with a failed boundary is dropped.
- **Strong modifiers** choose the segment and the point on the smooth loop, ties
  included, and insert the carried point into the noisy one. Among candidates
  that are carried the noise never changes the winner; a candidate whose
  transfer fails drops out, and the next one wins.
- **Weak zones** carry each boundary separately. Before anything is inserted, a
  policy, not a proven guarantee, tells some zones whose boundaries came out in
  the reverse order. It assumes that the points of a zone's true noisy arc lie
  within the window of the smooth zone, as its carried boundaries do; the window
  is an estimate. A short zone's true arc then stays within a circle around its
  smooth boundary, however much the noise lengthens it, while the arc round the
  rest of the loop leaves that circle wherever the loop spreads beyond it; its
  length tells nothing, as the noise may lengthen a stretch many times over. With
  a threshold of four windows on the smooth loop:
  - for a zone shorter than that, the circle lies around its smooth left boundary
    with the zone's length plus a window for radius. The zone is reversed when the
    noisy arc from its carried left boundary forward to its right one leaves the
    circle; otherwise it keeps its order when the arc from the right boundary
    forward to the left one leaves it; with both arcs inside, it is reversed when
    the first arc is the longer;
  - otherwise, for a zone whose rest of the loop is that short, the same holds
    around its smooth right boundary, with the rest's length plus a window for
    radius and the arcs swapped: the arc from the right boundary forward to the
    left one is the rest's;
  - otherwise the order is not checked; a transfer onto another part of the loop
    there is a limitation of the direct transfer.

  An arc leaves the circle when either carried boundary or a vertex of the noisy
  loop between them, across its start, lies outside; boundaries on one edge with
  the second behind the first make an arc round the whole loop. A reversed zone
  is reduced to its left boundary, a zero-length zone that types the one
  candidate there. This is a declared degradation, counted in the log: it
  happens, for example, for a piece of a convex corner's arc where the noisy
  loop cuts the corner inside it, since the lines through the corner then cross
  the noise behind it in the reverse order.

## Segment extraction

`extract_perimeter_segments()` turns one modifier's regions on one layer into
segments of the perimeter, each with its geometry and its position on the
source contour. Both modifier kinds consume these segments; the extractor is
told the modifier type so that it prepares only the data that type needs.

### Surface reconstruction

The surface is the set of points on the air side at the shift `d` from the
centerline. `build_surface_line()` builds it as a closed line whose every edge
remembers its source:

- **Shifted edge.** Each source edge moved along its normal into the air. With a
  constant shift along the edge, the shift is rounded once per edge, so the
  shifted edge is an exact integer translate of its source and keeps its
  parameters. Where a measured shift changes along the edge, the shifted edge is
  a chain of straight parts through the points moved by the shift at its
  vertices and at its measured interior points; each part maps linearly onto the
  range of parameters of the source edge between those points.
- **Arc.** Where shifted edges diverge (a convex vertex as seen from the air),
  an arc around the vertex whose radius is the shift at that vertex,
  approximated by chords with a sagitta of at most 40 units. Both shifted edges
  take that shift at the vertex, so the line stays continuous; with no shift
  there they meet at the vertex itself. A reversal of direction turns by half a
  circle.
- **Crossing.** Where shifted edges cross (a concave vertex) at a point `K`
  inside both of the parts meeting there, they end exactly at `K`: no part of
  them lies beyond it. A part trimmed this way covers only part of its range of
  the source edge, and its parameters are carried over proportionally. Both
  trims of one part must leave at least 4 units of it.
- **Bridge.** At any other concave vertex (shifted edges crossing beyond the end
  of one of them, as in a fillet finer than the shift), a straight edge between
  the ends of the full shifted edges, carried onto the vertex like an arc. The
  shifted edges keep their tails beyond the crossing.

Each crossing is the end of a **dead stretch**: the part of the centerline
around the vertex that has no surface of its own. It is recorded at once, with
the elements of the surface line that end and start at `K`. Resolving these
corners exactly matters on shallow corners, such as the many vertices of a
tessellated hole: there the parts of the full shifted edges beyond `K` stay
within micrometers of the surface, and a modifier boundary crossing them would
split the segment.

The surface line is not filtered further: every part of it counts as surface,
even where it comes closer than `d` to another part of the centerline. The
centerline is itself the model contour shifted inwards by half the line width,
so shifting it back by slightly less returns to that contour and does not fold:
a concave fillet of radius `r` on the surface is a fillet of radius `r + d` on
the centerline, and a sharp inner corner stays sharp and resolves exactly. The
line folds only on corners that are not resolved exactly: a very sharp inner
corner that the slicer bevels, a sharp change of the measured shift, or relief
finer than the shift (see [Known limitations](#known-limitations)). There the tails and bridges
of the line stay part of it, and the user's rule that a modifier enters the wall
by a clear depth keeps such corners out of play. A general offset of the polygon
could not replace the surface line: it rounds and cuts corners and loses the
link to source edges.

### Clipping

Regions whose bounding box does not overlap the perimeter's, enlarged by the
shift, are skipped. The surface line, as an open path closed at its start, is
intersected with each remaining region by `intersection_pl()`, which clips open
paths against an `ExPolygon` with its holes attached, using the nonzero rule.
Clipping an open line yields only pieces of the surface, so a modifier crossing
the whole object produces two separate pieces rather than a chord through the
body. Holes in a modifier and several regions of one modifier simply produce
more pieces. The line is cut at its start, so a piece crossing it arrives as two
fragments. A border that
only touches the surface can come back as a single point; such fragments carry
no coverage and are dropped before binding. With a shift of zero the
centerline's own clipping line is clipped instead, without offsets or contact
rules.

### Binding fragments to source edges

Clipper returns coordinates only. Insertion needs the source edge of every
point, and coordinates alone are ambiguous where a contour visits the same
point twice. Each fragment is therefore bound to the edges of the line it was
clipped from (the surface line, or the centerline for a shift of zero),
producing intervals: an edge index with a parameter range on that edge.

- **Exact path.** For fragments with interior points, the second point is used
  as an anchor that must equal a source vertex exactly. Clipping keeps the
  vertices of an open path unchanged, including collinear ones. The following
  points must match successive source vertices in either direction; later
  occurrences of the anchor are tried if a sequence does not match. Only the two
  end cuts are projected onto their edges.
- **Projection path.** Two-point fragments, and fragments the exact path cannot
  match, are bound by projection. The first source edge that holds both points
  of the first pair, with distinct parameters, establishes the edge and
  direction. Every following pair must continue on the same edge or cross to the
  neighboring edge at their actual shared vertex, in the same direction. A pair
  continuing on the same edge reuses the previous pair's parameter for their
  shared point, so the two projections of one point cannot differ.
- **Failure.** A fragment that cannot be bound continuously is rolled back and
  discarded. Earlier fragments and other fragments are unaffected. The failure
  is counted, logged and reported to the user (see
  [Diagnostics](#diagnostics-and-warnings)).

Two rare rounding cases are handled only after both paths have failed, so the
normal path never pays for them:

- **Cut beside a vertex.** When a modifier boundary crosses within about one
  coordinate unit of a source vertex, Clipper can place the cut at the vertex's
  height but a few units beside it. The end pair then collapses to the vertex's
  parameter or misses both neighboring edges. An end cut closer than the
  snapping radius to a vertex of the fragment's own chain is snapped to that
  vertex: either its neighbor in the fragment (the cut is a rounded copy of it
  and is dropped) or a vertex that shares a source edge with that neighbor. The
  neighbor wins whenever it is within the radius. Ends that are themselves source
  vertices and ambiguous choices are left unchanged. Binding is then retried
  once with the same strict rules, so a wrong candidate can only fail again.
- **Contact.** A fragment that still fails but is shorter than the snapping
  radius is accepted as a contact and binds nothing. Insertion would collapse it
  onto one point anyway.

Both outcomes are recoveries, not failures: they show no user warning but leave
a log marker.

### Carrying the surface onto the centerline

Intervals on the surface line are carried onto the centerline through their
source:

| Surface | Centerline |
| --- | --- |
| Shifted edge, parameters `t0..t1` | the same source edge, the same parameters (proportionally on a trimmed part or a part of a split edge) |
| Arc or bridge around a vertex | that vertex, as a **vertex point** |
| Both ends of a dead stretch covered | the whole centerline between them |

A point of an edge with a constant shift moves back by subtracting that edge's
integer shift, which is the same as following the normal. On an edge whose
shift changes, the point is the one at the same parameter of the source edge.

The last rule covers the crossing point `K` of a concave corner: when the
modifier covers the surface on both sides of `K`, the centerline around the
vertex, which has no surface of its own, joins them. When only one side is
covered, the segment ends under the modifier's crossing, outside the dead
stretch. An end counts as covered when coverage ends within the snapping radius
of it on the same edge of the surface line, so a modifier boundary passing
exactly through `K` covers it.

### Assembling segments

The intervals are sorted by edge and parameter. Intervals on the same occurrence
of an edge are united when they overlap or meet, by parameter or at the same
integer point; equal coordinates on different edges are never united. A vertex
point adds no edge: next to coverage that reaches its vertex it disappears, and
alone it forms a **zero-length segment**, a single point with no edges, where
the modifier reached the surface only around a convex corner. Its strong target
is the vertex in every mode, and as a weak zone it types the one candidate
there. A
parameter of 1 is stored as parameter 0 of the next edge, so intervals on
adjacent edges meet exactly at their shared vertex. Consecutive intervals that
meet form one `PerimeterSegment`, and the last segment is joined with the first
when they meet at vertex zero, undoing the artificial cut of the clipping line.

Each segment keeps its polyline, the source edge of every polyline edge, and its
begin and end positions on the source contour.

### Full containment

A modifier that covers the whole perimeter has no boundaries on it. The policy
follows seam painting, where a whole perimeter can be painted in either color:

- **Seam Enforced** types the whole perimeter, like a perimeter painted green all
  round, with subdivision applied as described under [Weak modifiers](#weak-modifiers).
- **Seam Neutral** types the whole perimeter Neutral, like an unmarked perimeter,
  clearing painting and lower zones.
- **Seam Blocked** types the whole perimeter Blocked, like a perimeter painted
  red all round, overriding painting and lower zones. The seam cannot avoid such
  a perimeter: the seam placer compares Blocked candidates with each other like
  Neutral ones, so the seam is placed as on an unmarked perimeter. If the
  perimeter stays Blocked after all weak zones, it is reported in the user
  warning (see [Diagnostics and warnings](#diagnostics-and-warnings)), so the
  user can check the modifier.
- **Seam Center, Left and Right** are skipped with the full-containment warning:
  there is no intersection to place the point on.

Enforced, Neutral and Blocked take part in the usual priority order (see
[Weak modifiers](#weak-modifiers)).

The perimeter is fully contained when the united intervals cover every source
edge from parameter 0 to 1, which is the case when the modifier covers the whole
surface: every shifted edge and every dead stretch. A modifier that covers the
whole centerline but leaves part of the surface outside gives ordinary segments.
An uncovered part of a single arc carries onto one vertex, a gap of zero
length, and so still counts as full containment. A modifier boundary that merely
touches the perimeter counts as well:

- At a vertex or on an axis-aligned edge, clipping splits the line exactly at the
  touch, the pieces meet at one point, and the coverage is complete.
- On an inclined edge the touching point is usually not representable on the
  integer grid. The boundary pokes a few units across and leaves a real gap, so
  a single segment covers everything except that gap.

Weak insertion would collapse such a segment's boundaries onto one vertex and
turn the intended zone into a single candidate, and strong would put the seam at
the touch. A single segment is therefore also full containment in the cases
where insertion collapses it, exactly up to edges shorter than 2 µm:

- the uncovered length from its end to its begin is below 1 µm, or
- the gap spans one vertex, or starts at a vertex and ends on the next edge, and
  both ends lie within 1 µm of the vertex that ends the first gap edge, since
  each end then snaps onto it from its own edge.

A cheap filter runs first: both cases bring the segment's ends within 2 µm of
each other. A zero-length segment covers nothing and is never full containment,
although its ends coincide.

## Strong modifiers

For a strong modifier, the extractor prepares each segment's target point
before anything is inserted, together with the source edge it lies on:

- **Left:** the segment's first point.
- **Right:** the segment's last point.
- **Center:** the point at half the segment's arc length.

Arc length is the sum of Euclidean edge lengths, not the chord or a vertex count.

`insert_strong_seam_point()` selects the longest segment of the first modifier
that has one; a zero-length segment loses to any other. Exactly equal lengths are resolved by the prepared target points:
greater bed Y first, then smaller X; a complete tie keeps the first segment.
Slice coordinates already include instance rotation and have the bed axes;
centering and XY translation do not change this order. Nearly equal lengths are
not treated as equal, so exact ties occur mainly on axis-aligned geometry.
Geometrically equal segments, such as a symmetric modifier crossing both faces
of a thin wall, differ only by rounding noise that varies between layers, so
the chosen face may alternate. This is accepted deliberately: such a modifier is
ambiguous by itself: more than one segment raises the "multiple intersections"
warning. The user should make the modifier cross the perimeter once.

The selected point is inserted on its source edge. A point within 1 µm of an
existing vertex is snapped to that vertex. Helper points are added 1 µm on both
sides of it, except on an adjacent edge shorter than 2 µm, which already bounds
the distance.

When the candidates are built, the candidate at the inserted point is the only
enforced one and becomes the central enforcer; every other candidate is blocked.
Every seam position mode therefore selects it. Alignment and random placement
can still move the final position, so after alignment
`restore_precise_seam_positions()` writes the exact point and its index back
into every perimeter that has a strong seam.

## Weak modifiers

`collect_weak_modifier_segments()` extracts the segments of every weak modifier
before the polygon is modified, so all positions refer to the same contour. Each
segment becomes a zone with a type and two boundaries, kept in application
order, lowest priority first. Full containment of a weak modifier becomes a
whole-perimeter zone at its place in that order: it has no boundaries and takes
part in no insertion or helper step below. The boundaries
carry their positions on the source contour; these remain as provenance after
insertion and are not indices into the modified polygon.

`prepare_weak_modifier_segments()` then changes the polygon:

1. **Boundary insertion.** Insertion events are sorted by decreasing source edge
   and parameter, and the polygon is modified from its end towards its start. A
   pending boundary's source index therefore stays valid. Vertex zero has the
   canonical position `(0, 0)` and is
   processed last, and a point on the closing edge is appended rather than
   inserted at index zero. A boundary within 1 µm of either endpoint of its
   current edge, an original vertex or a boundary inserted earlier, is snapped to
   that point, so coincident boundaries share a vertex. A zone narrower than
   1 µm collapses into a single vertex.
2. **Helper points.** A helper point is added 1 µm outside every boundary,
   unless the edge there is shorter than 2 µm, which already bounds it. The
   helpers keep the edges at a boundary short, so a seam placed along such an
   edge stays close to the boundary. Coincident boundaries share their helpers.
3. **Enforced subdivision.** Zone types are resolved for the polygon's edges in
   priority order. The edges of a zone are those from its left boundary up to,
   but not including, its right boundary; a whole-perimeter zone types every
   edge. Enforced edges longer than `SeamPlacer::enforcer_oversampling_distance`
   (0.2 mm) are subdivided into steps of at most that length; shorter edges and
   existing vertices are kept.
   The regular seam placer then chooses the seam as for painted seams.

When candidates are built, painting assigns their types first.
`apply_weak_modifiers_to_perimeter()` then overwrites the types of the
candidates between the boundaries of each zone, both boundaries included,
lowest priority first; a whole-perimeter zone types every candidate. Blocked
and Enforced zones therefore take precedence over painting, and Neutral clears
painting inside its zone.

## Numeric tolerances

Coordinates are integers in scaled units: 1 nm by default, and 10 nm when a bed
larger than 2147 mm switches `SCALING_FACTOR`. Both Precise Seam tolerances are
deliberately defined in units rather than physical distances. Clipper truncates
cuts to whole units at any scale, so the on-edge tolerance must follow the unit; the
snapping radius scales with it to keep its margin over single-precision
candidate coordinates, which are coarser on large beds. Distances quoted in
this document in nanometers and
micrometers assume the default unit; on large printers they are ten times
larger. The enforced subdivision step is a physical distance and stays 0.2 mm.

| Value | Role |
| --- | --- |
| `MACHINE_PRECISION_SQUARED` (2.5 units², about 1.6 nm) | A point lies on an edge if it is this close. It absorbs Clipper's truncation of cuts to whole units (under √2 units from the edge) and never bridges a real gap: a one-unit uncovered gap stays a gap. |
| `TOLERANCE_LINEAR` (1000 units, 1 µm) | Insertion snaps points this close to an existing vertex, and helper points are placed this far from boundaries. The same radius bounds the rounding fallback, contacts and the sub-micron full-containment rule, so those decisions match what insertion would produce anyway. It is also the reach of a dead-stretch end, the smallest surface shift, and the agreement of measured shifts with the nominal shift and with a straight change along an edge. |
| `SURFACE_MARGIN` (5000 units, 5 µm) | The reconstructed surface lies this far inside the model surface, so a modifier flush with a model face does not act. |
| `SURFACE_ARC_SAGITTA` (40 units) | Maximum sagitta of the chords of surface arcs. |
| `SHIFT_SAMPLING_STEP` (0.5 mm) | Largest spacing of shift measurements along an edge, the step at which elephant foot compensation is resampled. |
| Slicing resolution plus 1 µm | Widens the accepted range of measured depths: perimeters are built on a contour simplified by the resolution. |
| `enforcer_oversampling_distance` (0.2 mm) | Maximum step of enforced subdivision. |

Raising the on-edge tolerance would not help with cuts beside a vertex: more
points past a vertex would be clamped to its parameter and collapse. Lowering it
would reject ordinary rounded cuts. The snapping radius is kept far above
clipping precision for robustness: seam candidates hold single-precision
coordinates, whose step is about 8 to 15 nm at typical object coordinates
(about 0.25 µm 3 m from the object's centre, on large beds only), and
weak boundaries and the strong point are located among the candidates by those
coordinates, so distinct points must stay clearly distinct. 1 µm is also far
below printing precision.

## Diagnostics and warnings

One `PreciseSeamWarnings` instance is shared by all objects and layers of a
`SeamPlacer::init()` call. After all objects are processed, `SeamPlacer::init()`
prepares at most one warning text, available through `precise_seam_warning()`.
G-code export issues it as one non-critical warning with the ID
`SlicingPreciseSeamWarning`. It is a single line, "Precise Seam: <causes>. Seam
placement may differ from expected.", because the export warnings dialog shows
only the first line of each warning. Repeated warning events replace the
notification instead of appending to it. Except for the "covers an entire
perimeter" and "had no effect" causes, the causes name the modifier types
involved, as the menu names them, in menu order and each type once, for example
"(Seam Left, Seam Enforced)".
The causes are:

- **failed to process some intersections (types):** at least one fragment was
  discarded by binding. Other segments remain usable.
- **multiple intersections with a perimeter, only one was used (types):**
  a Seam Center, Left or Right modifier had more than one segment on a
  perimeter (see [Strong modifiers](#strong-modifiers)).
- **a perimeter is fully inside a modifier, the modifier was not applied to it
  (types):** a Seam Center, Left or Right modifier was skipped for a
  perimeter (see [Full containment](#full-containment)).
- **Seam Blocked modifier "<name>" of "<object>" covers an entire perimeter:**
  a Seam Blocked modifier covered a whole perimeter (see
  [Full containment](#full-containment)). Only perimeters whose candidates are
  all Blocked after all weak zones contribute to this warning, and only through
  the zone in effect there: `apply_weak_modifiers_to_perimeter()` checks the
  final types and flags the modifier of the highest whole-perimeter zone when it
  is Blocked. A lower whole zone is overwritten by it and is not reported. Only
  the first flagged modifier in print and volume order is named, followed by
  "(N in total)" when there are several. The type name reuses the menu's
  translation.
- **modifier "<name>" of "<object>" had no effect on the seam:** a modifier was
  evaluated on at least one perimeter and never gave a segment, full containment
  or a discarded fragment. Only the first such modifier in print and volume order
  is named, followed by "(N in total)" when there are several.

  Only the effect is certain, so the text names no cause. A modifier is
  evaluated only when its turn comes: on a perimeter where a higher strong
  modifier placed the seam, lower strong and all weak modifiers are not
  evaluated. A modifier that was never evaluated is not reported, since nothing
  is known about it. One that was overridden on the perimeters it crosses and
  evaluated only on others it does not reach is reported, although it does reach
  the model surface; a cause given in the text would mislead there. A point
  contact gives no segment and does not count as reaching the perimeter.

The log records the following diagnostic markers:

- `[PreciseSeamIntersectionFailed]` for a discarded fragment, with object,
  modifier, layer, height, fragment and failing pair, the failure reason and
  point counts.
- `[PreciseSeamFragmentRecovered]` for a recovery, with `outcome=bound` or
  `outcome=contact`, the same location fields and the original failure reason.
- `[PreciseSeamNoEffect]` for every modifier of the "had no effect" cause, with
  the object and modifier names. Unlike the user warning, the log lists all of
  them.
- `[PreciseSeamFullyBlocked]` for every modifier of the "covers an entire
  perimeter" cause, listed the same way.

Failures and recoveries are counted separately. The first 10 of each per
`init()` call are logged in detail, in parallel processing order; if a limit is
exceeded, one summary marker reports the total.

## Known limitations

- **Where the shift is not measured, or only approximately.** On layers
  without elephant foot compensation the classic generator keeps the nominal
  shift, so its narrow loops, printed slightly thinner, get a surface a few
  micrometers outside the model. A noisy loop without its smooth centerline
  (sliced data loaded from a cache) gets a surface that repeats the noise. A
  measured point moves along the normal of its
  edge by the depth projected on that normal: where elephant foot compensation
  changes along a wall, the centerline's edges are inclined to the outline, and
  the surface falls short of it by `D * (1 / cos a - cos a)` for an inclination
  `a`, a few micrometers for usual changes. A closed Arachne loop of
  a single line (a ring thinner than two lines) gets a surface on its contour or
  hole side only.
- **Fuzzy skin.** The crossing nearest to the smooth point is taken without
  knowing which part of the noisy loop a noisy point came from: with steep noise,
  or where another part of the same loop lies within the window (a thin part, an
  acute corner), the nearest crossing may belong to another place than the one
  the smooth point was textured into. Points carried without the line
  (approximate outcome) do not lie on it, and weak zones whose boundaries reverse
  are reduced to one point. The window is estimated from the settings. A transfer
  fails only when neither a crossing nor a nearest point of the noisy loop lies
  within the window, so a boundary that the noise moved farther may be carried
  onto another crossing or point within it instead; inner points of a noisy arc
  farther than the window can mislead the order policy. The order of a
  weak zone's boundaries is checked only where the zone or the rest of the loop is
  shorter than four windows; where both are longer, boundaries carried onto
  another part of the loop are not noticed. A loop that lies wholly within the
  circle of that check (its spread comparable with the window) leaves the
  decision to the lengths of the two noisy arcs: noise lengthening one of them
  unevenly may then reduce a correct zone or miss a reversal. When both smooth
  arcs are shorter than four windows, the zone's rule takes priority. Sliced data loaded from a cache keeps only the
  extrusions, without the untextured centerline: such loops keep the noisy
  surface and the nominal shift, so the same project may get another seam after
  loading. The tests only clear the field by hand; they do not go through
  writing and loading the cache. A closed thin wall that the classic generator
  builds outside its loops has no untextured centerline either.
- **Grazing contacts.** Contacts are taken as clipping returns them, without
  tangency rules, so boundaries that only graze the surface are the user's
  responsibility. Several near-touches on inclined edges can leave several
  segments separated by gaps of a few units; their zones then cover nearly the
  whole perimeter instead of being treated as full containment.
- **Concave corners not resolved exactly.** At a very sharp inner corner that
  the slicer bevels, at a sharp change of the measured shift, with a noisy loop
  without its smooth centerline whose points are closer than the trim of a corner
  (`d * tan(turn / 2)`) or with
  artificially fine relief, the surface line keeps its tails and bridges. A modifier boundary
  closer than `d` to such a corner can give an extra segment, the
  multiple-intersection warning and a local distortion of the segment of up to
  `d`; a modifier in the air closer than `d` to such a corner can act. `d`
  bounds the distortion, not the movement of the seam: if the distortion changes
  which segment of a strong modifier is the longest, the seam moves to another
  segment, which may lie at any distance.
- **Walls between regions.** XY compensation is taken once per object, for its
  outer contours and its holes. Where an external perimeter runs along the
  boundary between two regions of one object (regions whose perimeters are
  generated separately), slicing compensates that boundary differently: a
  negative compensation does not move it, and a positive one moves it according
  to the order of the regions. With a nonzero compensation the reconstructed
  surface of such a wall is therefore off the real region boundary by the
  amount of the compensation, and a modifier near that boundary may act or not
  act accordingly. Restoring it exactly would need the shift actually applied to
  each part of the loop, since one loop can contain both such a boundary and a
  compensated outer contour. The measured shift does not change this: the
  islands do not contain such a boundary, so its points fall outside the
  accepted range and take the nominal shift, except near the place where the
  boundary meets the outer contour.
- **Self-touching perimeters.** Extraction keeps distinct visits of one
  coordinate apart through its source-edge bindings, but the consumers locate
  inserted points by coordinates. A weak zone is typed and subdivided from the
  first vertex with its boundary coordinate, while boundary helpers are added at
  every such vertex. A strong point marks every candidate at its coordinate as
  enforced, and the last one is restored after alignment. If a boundary or a
  strong point falls exactly on a repeated coordinate, a zone may therefore start
  from another visit, or the seam may start at another visit of the same point.
  Carrying visit identity through insertion, refinement, candidates and
  restoration would touch the whole pipeline, so it is not done for this rare
  geometry. Overlapping source visits are likewise outside the binding contract.

## Integration with the application

### Other seam settings

- Precise Seam takes part only in outer and hole perimeter seam placement. In
  spiral vase mode the seam placer is not used for perimeters, so the modifiers
  have no effect.
- Scarf seams, the seam gap and wiping start from the chosen point exactly as
  they would from an ordinary seam.
- Seam painting acts only from model parts, the volumes the seam gizmo shows and
  edits, and from negative volumes. Painting retained on a volume after a change
  from part to a Precise Seam, ordinary or support modifier is ignored. A type
  change back to a model part reactivates any retained painting.
  Negative volumes keep it on purpose: painting a
  part and turning it into a negative volume is the only way to paint the wall
  of the hole it cuts. That painting still affects the seam but is invisible in
  the gizmo and cannot be edited there; this is known technical debt.
  If painting them is ever made editable, G-code invalidation must track it too:
  `model_custom_seam_data_changed()` checks model parts only.

### Model storage and 3MF compatibility

Projects must stay readable by earlier releases, and a Precise Seam volume must
not change a print there. Both 3MF writers therefore store it as an ordinary
parameter modifier: `modifier_part` in the Bambu-format part subtype, and
`ParameterModifier` together with the legacy `modifier` flag in the
Prusa-format volume metadata. The seam mode is written separately under
`precise_seam_type`, using the names from `ModelVolume::type_to_string()`
(`precise_seam_center` and so on).

On load, the mode is applied after all other volume metadata, regardless of XML
key order, and only when the base type is a modifier. A missing or unknown mode
leaves an ordinary modifier, and seam metadata on any other base type is
ignored. Files that stored the seam mode directly as the volume type still load.
A project saved again by an earlier release loses the seam mode for good: the
volumes stay ordinary modifiers without settings.

A Precise Seam volume keeps any per-volume settings it had as a part or
modifier, but they are inactive and the object list shows no settings item for
it. The writers prefix these keys with `precise_seam_config:`, so an earlier
reader drops them as unknown options and loads a modifier without settings,
which has no effect on the print. The current reader restores the keys only when
the volume ends up as a Precise Seam type, so the settings return when the user
changes the type back.

### Print invalidation

`Print::apply()` compares the Precise Seam volumes of each object by type, ID
and transformation. Adding, removing, moving, reordering or retyping one cancels
background processing and invalidates only `psGCodeExport`; the sliced layers
are kept. `model_volume_list_update_supports_and_seams()` then brings the
support and Precise Seam volumes of the print's model copy in line with the new
model in one pass. A volume may switch between these two families, since neither
affects object slicing; such a switch also changes the support volumes, so the
support step is invalidated as well.

A conversion to or from a part or an ordinary modifier changes the solid and
modifier volume lists and reslices the object as before. The volume keeps its
ID across the type change, so the region cache treats a former support or
Precise Seam volume that became a part or modifier as new, since it was never
cached.

Removing the last helper of a single-part object reslices it, as removing any
last modifier would.

### User interface

- *Add Precise Seam* in the object menu creates a Center modifier from a
  primitive or a loaded mesh. Text and SVG volumes cannot become Precise Seam
  modifiers: the menu does not offer it, and `ObjectList::set_volume_type()`
  refuses the change.
- *Change Type* has a single *Precise Seam* entry. It converts other volumes to
  Center and keeps the mode of volumes that are already Precise Seam. The
  *Precise Seam Type* submenu appears only when every selected item is a Precise
  Seam volume, including settings rows that resolve to one, and sets the chosen
  mode on all of them.
- Each mode has its own icon in the object list and its own color in the 3D
  view, at 60% opacity: warm orange, gold and dark orange for Center, Left and
  Right; green, red and gray for Enforced, Blocked and Neutral. The three strong
  colors are close shades of one orange because all three mark strong
  modifiers; the object list icons tell the modes apart.
- Precise Seam volumes have no filament and cannot be pasted into SLA objects.
  Python plugins see them as `ModelVolumeType` values and through the
  `is_precise_seam*()` methods.

## Implementation and verification

- [PreciseSeam.cpp](../../src/libslic3r/GCode/PreciseSeam.cpp) implements the
  modifier cache, perimeter preparation, segment extraction and binding, strong
  selection and insertion, weak-zone preparation and application, and position
  restoration. [PreciseSeam.hpp](../../src/libslic3r/GCode/PreciseSeam.hpp)
  declares the contracts; [PreciseSeamInternal.hpp](../../src/libslic3r/GCode/PreciseSeamInternal.hpp)
  exposes the binding internals to tests.
- [SeamPlacer.cpp](../../src/libslic3r/GCode/SeamPlacer.cpp) fills the cache,
  normalizes perimeters, calls both consumers while gathering candidates,
  restores strong positions after alignment and prepares the warning text, which
  [GCode.cpp](../../src/libslic3r/GCode.cpp) issues during G-code export.
- [Model.hpp](../../src/libslic3r/Model.hpp) defines the types and their order,
  [PrintApply.cpp](../../src/libslic3r/PrintApply.cpp) handles invalidation, and
  [PrintObjectSlice.cpp](../../src/libslic3r/PrintObjectSlice.cpp) slices single
  volumes into structured regions. [bbs_3mf.cpp](../../src/libslic3r/Format/bbs_3mf.cpp)
  and [3mf.cpp](../../src/libslic3r/Format/3mf.cpp) store them.
- [GUI_Factories.cpp](../../src/slic3r/GUI/GUI_Factories.cpp) and
  [GUI_ObjectList.cpp](../../src/slic3r/GUI/GUI_ObjectList.cpp) provide the menus,
  type changes and ordering; [3DScene.cpp](../../src/slic3r/GUI/3DScene.cpp)
  defines the colors.
- [Segment extraction tests](../../tests/libslic3r/test_precise_seam.cpp) cover
  surface reconstruction (convex and concave corners, holes), crossing the surface without reaching the centerline, modifiers
  inside the body, arcs giving zero-length segments, dead stretches covered or
  not, contacts and flush faces on straight, inclined and tessellated walls,
  full containment of the surface versus the centerline (also with part of one
  arc uncovered), a fully covered concave fillet finer than the shift, reversals, nearly collinear vertices,
  vertex points at vertex zero and on holes, the bounds of the surface shift,
  and independence of contour origin and direction. A shift that changes along
  the perimeter is checked at vertices, arcs and a vertex without shift, at a
  resolved concave corner and on a split long edge swept by a boundary; its
  measurement against an outline at the nominal depth, deeper, beyond the
  accepted range, with compensation inside and outside the outline, within the
  margin, and with a step along an edge. Fuzzy skin: the transfer onto hand-made
  noisy loops across a straight edge, at an acute corner, with several crossings,
  along an edge on the line, and its three outcomes; the surface pairs of segment
  ends, of a piece of arc, of separate pieces of one arc, across the start of the
  loop, and of Center inside an edge, at a vertex, at a collinear vertex (also
  vertex zero and a changing shift) and in a dead stretch; a loop keeping its
  untextured centerline when copied, reversed or turned. A boundary swept in sub-micron steps across shallow concave
  vertices of tessellated holes and notches must move a single segment end
  monotonically. On the centerline they cover
  clipping and binding: holes and components, contour origin and reversal,
  repeated coordinates, collinear vertices and rounding, rollback and the
  diagnostic limits, the rounding fallback on synthetic and real Clipper
  fragments, contacts, and full containment including touches and sub-micron
  gaps on inclined edges and around vertices.
- [Precise Seam tests](../../tests/fff_print/test_precise_seam.cpp) cover the
  consumers: strong targets in every mode, including a midpoint on an existing
  vertex or the closing edge and a zero-length segment, longest-arc selection
  (also of a longer segment over a zero-length one) and tie order in bed axes,
  zero-length weak zones of every type, with fuzzy skin strong points chosen on
  the smooth loop and inserted into the noisy one or discarded when they cannot be
  carried, and weak zones short, zero-length at a convex corner, reversed (for
  every type, also on a loop that lies wholly within the circle of the order
  check), across the start of both loops, long or short with noise
  lengthening only their part of the loop and almost all round, also with noise
  lengthening only the short rest (each for every type), priorities, weak boundaries that coincide or share an edge, enforced
  subdivision, whole-perimeter weak zones with painting and priorities, weak
  zones over painting's oversampled candidates, the warning type masks, usage
  tracking for the "had no effect" and whole-perimeter Blocked warnings, volume
  sorting of strong and weak groups, restoration of strong points after
  alignment, raft layer indexing and structured slices. End-to-end tests slice
  a real object with Precise Seam
  volumes and check the outer wall starts in the exported G-code: every strong
  mode under several seam positions and with a raft, a modifier reaching only
  0.1 mm into the model, contour and hole walls under positive and negative XY
  compensation with both generators, a first layer with a wider line, the
  measured shift on elephant foot layers with both generators (the same
  modifier depth as above, flush and slightly deeper modifiers, a hole, a thin
  part they cannot shrink fully, a raft), XY compensation together with elephant
  foot compensation in one region and in several, an Arachne wall narrower than
  its line, fuzzy skin on contours or holes (measured on the smooth centerline;
  which loops keep it; with sine noise a modifier acts at the model surface, not
  at the noise, also on part of a wall; a weak zone; Arachne's combined mode with
  the smallest noise; elephant foot compensation in one region and in several; a
  closed classic thin wall; loops whose untextured centerline is cleared by hand,
  as loading cached data leaves them), a modifier
  crossing only an open thin wall, Enforced and Blocked zones,
  a perimeter blocked all round under every seam position, a modifier with a
  hole, and the user warning.
- [Seam placer tests](../../tests/fff_print/test_seam_placer.cpp) cover
  enforced-patch selection independent of the contour start, fully painted
  contours, duplicate removal, and `Print::apply()` synchronization through type
  changes and restored model snapshots. The duplicate-removal test also checks
  the "had no effect" warning text prepared by `init()` for a helper that never
  reaches the loop. Further tests check that adding, moving, retyping or
  removing a Precise Seam volume invalidates only G-code export, and that seam
  painting acts only from model parts and negative volumes, including after a
  type change back to part.
- [3MF tests](../../tests/libslic3r/test_precise_seam_3mf.cpp) cover the round
  trip of every mode and of inactive settings, attribute escaping, and the
  metadata combinations that restore a seam mode.
  [Plugin tests](../../tests/slic3rutils/test_precise_seam_plugin.cpp) cover the
  Python bindings.
