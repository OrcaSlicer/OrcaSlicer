# Seam alignment on the loop — High Level Design

## Purpose and scope

SeamPlacer selects candidates, connects neighboring layers into strings, fits each string and
places the resulting seam on its own contour. Painted ranges are reconstructed separately for
green enforcers and red blockers. These stages do not alter candidate ranking.
String construction and fitting apply to Aligned, Aligned Back and Back (`spRear`).
Nearest and Random bypass this stage, but share the paint reconstruction rules.

## Concepts

- **Candidate (C).** The seam candidate chosen for a perimeter in a string. Candidates are the
  polygon vertices plus points inserted near paint (see Painted ranges), so the chosen candidate
  can move in discrete steps from layer to layer.
- **Fitted point (F).** The value of the string's fitted curve at the layer height. It need not
  lie on the loop: where the loop size changes quickly between layers, as near the bottom of a
  sphere, it lies up to a few millimeters off it.
- **Measured angle.** `local_ccw_angle` of a candidate. It is measured at polygon vertices on arms
  at least one nozzle diameter long; inserted points have angle 0.
- **Blend.** `target = t·C + (1 − t)·F`. The angle weight `t = min(1, (|angle C| / 55°)^3)` pulls
  the seam onto sharp corners; at 55° and above the seam is the candidate itself.
- **Enforced run.** The maximal sequence of consecutive Enforced candidates of the perimeter that
  contains C, or the whole loop when every candidate is Enforced. With exact painted ranges it
  follows its geometric bounds and surface proximity; otherwise it approximates the painted area with the candidate spacing.

## Painted ranges

The paint is a set of small triangles on the model surface. Its **contour** is the set of their
edges that belong to one painted triangle only, taken from the painted triangles with their T-joints
split (`get_facets_strict`), so that edges inside the painted area pair up. Each contour edge keeps
the direction from it into the paint. The contour is built once per object and stored in an AABB
tree.

Green and red paint use separate instances of the same contour and interval reconstruction.
An interval bounds paint membership but does not establish it: every classified candidate must
also lie within one line width of a triangle of the corresponding paint. A contour can leave the
painted surface between projected endpoints, especially at concavities and nearby surface folds.
The same proximity check used by the fallback prevents such a detour from acquiring synthetic
paint; exact interval bounds still exclude the fringe beyond the projected ends.
Red ranges classify candidates as Blocked, including the boundary itself (1 µm membership
tolerance), overriding green paint at an overlap. Inserted samples include both red ends, a
range midpoint, a gap midpoint and permitted guard samples up to 2 µm outside the red ends.
The guard distance is reduced to one quarter of a shorter gap; gaps at or below the membership
tolerance can remain closed. Candidate marks closer than 1 µm merge.
These adjacent permitted samples limit fitted and optimized movement to the geometric red
edge, with the small outward guard, rather than to a distant original gray vertex. Red ranges add
no penalty and do not change candidate ranking. Precise Seam modifiers apply after paint
classification and take priority over it.

Random placement samples an outgoing edge of its selected candidate. On loops with exact
green ranges, an Enforced candidate followed by a non-Enforced candidate contributes no outgoing
edge length: the endpoint belongs to the paint, but the following segment leaves the permitted run.
This also respects weak modifiers that retype part of a painted run as Blocked or Neutral.
On loops with exact red ranges, a permitted candidate followed by a Blocked candidate contributes no outgoing
edge length, so Random cannot sample toward the closed red boundary. A zero-length selected
edge keeps the candidate position.
The cumulative-length selection stops at the last viable edge and clamps its
remaining distance to that edge, preventing float rounding from overrunning the
candidate list or the permitted segment.

Exact green and red ends bound the seam center position without expanding the painted range
along the contour by the line width. A seam can therefore reach a red edge with no line-width
clearance. Paint can be extended to request a larger clearance; the boundary does not
guarantee that the finite-width extrusion or a seam blemish stays entirely outside red paint.

If boundary crossings cannot be paired, the corresponding color falls back to the line-width
proximity classification, including its possible fringe beyond the paint edge. The fallback is
independent for each color and loop. For red this includes paint not crossing the layer,
ambiguous contours and a completely red surface without boundary crossings. In the all-blocked case
best-effort placement still produces a seam; red paint does not suppress extrusion.
An exact red reconstruction does not suppress proximity-based green oversampling when green
boundaries remain uncertain.

On a layer, the contour edges crossing the slicing plane give the exact ends of the painted band.
Each crossing within the line width of the loop is moved to its closest loop point; the direction
into the paint, against the loop direction there, tells whether paint lies ahead or behind. At a
vertex the loop direction is the mean of the two edges, skipping edges shorter than 1 µm such as
the repeated closing point and the shared ends of extrusion paths, so the result does not depend on
where the loop starts. Along the loop, ends where paint begins and ends must alternate; each
pair bounds a **painted range**. Such a loop has **exact ends**:

- a vertex is Enforced if it lies in a range (within 1 µm);
- points are inserted at the range ends, at the range middle and every 0.2 mm from the range begin,
  and one in the middle of each gap between ranges, so that ranges with no vertex between them stay
  separate patches;
- the central candidate of the longest patch is the middle of its range, unless the patch has a
  sharp corner, which keeps priority.

Before generating candidates, a fully painted closed surface is treated like an unpainted surface:
it imposes no choice of seam position. A loop on a partly painted object can also be normalized when
there are no exact range ends, no active Precise Seam modifier, no non-green facet bounding box in
the loop's XY box expanded by line width and its Z slab of the same reach, and every entire loop edge
is proven within paint reach. Neutral and blocked facets both prevent this local proof. Segment
coverage uses the triangle inequality at its midpoint and at most eight bisections; failure retains
the ordinary conservative paint treatment. This is a sufficient test, not a claim to recognize every
fully covered contour near unrelated surfaces.

A normalized loop keeps the original candidates, without paint oversampling, Enforced status,
central enforcer, extra fitting or positional weights, painted-corner blending or paint fixation. Sharp-angle and Precise Seam rules remain authoritative. Whole-surface painting and no
painting therefore feed identical candidates into alignment.

Otherwise, which includes paint not crossing the layer (a dot between layers, the top and bottom of
a band), ends that do not alternate and a contour running along the
loop, a point is Enforced if paint lies within the line width of it, and edges near paint get points
every 0.2 mm from their start vertex. Such a run reaches past the paint by up to the line width,
depending on the vertices, and its central candidate is the middle one by count; on a smooth band
both shift the seam off the middle of the paint.

## Seam strings

A string starts at a seam and is extended upward layer by layer while the next layer has a seam
within reach. After the upward search ends, the downward search begins at the layer immediately
below the start. This keeps a matching neighboring seam in the same aligned string.

## Geometric contour continuation

Seam strings resolve the next layer's boundary before comparing seam preferences.
Each perimeter retains whether it bounds material or a hole, its layer-local index,
and immutable candidate geometry. Occupancy does not participate in correspondence: the radius search skips occupied
members of the permitted set without expanding it to unrelated walls.

Each layer builds an AABB tree of contour bounds once. Nearby-contour queries,
fast bounds matching, material containment and boundary proximity use this index
before applying their exact predicates. Query results are sorted by the
original contour index, preserving deterministic tie-breaking. Index construction
checks cancellation between layers. The index avoids scanning unrelated contours
for each seed; overlapping global bounds can still require many exact checks.

Only closed extrusion loops provide a reliable boundary role. Open paths and
collection fallbacks retain unknown geometry rather than deriving a physical role
from an artificial closing edge. An unknown source, or an unknown path whose
bounding box intersects the local query box in either layer, leaves the radius
search unrestricted. This conservative box test also covers the material probes;
it may disable the filter even when the path's segments are farther away.
Reverse confirmation applies only to candidates that pass the applicable distance
and normal checks, plus material checks on the local path outside the noise band.
Unknown geometry encountered around such a candidate's foot during reverse confirmation
also leaves the entire radius search unrestricted, rather than discarding only
that candidate. A reliable reverse rejection still excludes the candidate.
Unknown paths outside these query boxes do not disable reliable correspondence.

Bounding-box extents within one nozzle diameter plus a quarter-diameter reserve
identify a potential fast match of the same boundary type. Acceptance also requires
a closest segment within one eighth of the source line width, consistent air-facing
normals, no competing near-coincident local boundary, and a reverse set containing
the source. Thus the inexpensive path certifies only a near-coincident wall, rather
than treating global bounds and the full search radius as sufficient evidence.

The fallback uses the caller's KD search radius, normally four seed-line widths,
for nearby boundaries of either type. With unchanged boundary type it finds the
nearest segment whose air-facing normal is compatible with the source, so the
opposite side of a thin contour cannot hide its facing side. Across a type change
the closest patch itself must agree; choosing another side can confuse translated annuli.
An open exterior wall can become a hole when a slit closes; boundary type alone
cannot reject that physical continuation. It checks air-facing
normals within 60 degrees and samples the connector between closest boundary points.
Away from boundary noise, the connector must lie in material of exactly one adjacent
layer, representing a swept wall rather than a crossing through shared material or
air. Three samples are a bounded heuristic, not a topological proof.

Each local candidate must map back to a set containing the source boundary. A candidate
is discarded only if another has both a distance advantage of at least a quarter of
the source line width and a normal-angle advantage of at least five degrees. Equal or
conflicting criteria retain multiple candidates; seam preferences choose within
this set. Air-facing normals and material checks distinguish wall sides without imposing
an invariant exterior/hole label across topology changes. They are geometric gates,
not numerical preference weights.

If no geometric candidate survives, the radius search is left unrestricted. Continuation
then follows seam preferences alone and can switch walls where geometry cannot resolve
the case. A nonempty geometric set is never
expanded because its contours are occupied or fail later seam preference checks.

Correspondence queries are cached per source vertex, direction and search radius
for alternative string starts. Geometry construction and the seed loop check
cancellation to keep large multi-island objects interruptible. Correspondence does not alter candidate
preference weights or their tolerance.

## Final position

For each member of a string:

1. **Sharp candidate** (`t = 1`): the seam is C. This is the cheapest check and comes first.
2. **Painted corner pull** (Enforced C only): `t = max(t, 0.4·k)` with the corner influence
   `k ∈ [0, 1]` below. Retaining the pull near measured corners protects corners; fading it on
   smooth contours reduces transmission of discrete candidate steps into the seam. If C's own angle
   reaches `A1`, `k = 1` without a search.
3. **Place on the loop.** The target is mapped onto the loop, within the allowed arc around C.

### Corner influence

`k` is the largest contribution `a_j · w_j` among candidates `j` of the same perimeter within `R1`
of C along the loop, `d_j` being the distance along the loop:

- `a_j` rises smoothly from 0 at a measured angle of `A0` to 1 at `A1`, so weak bends count partly
  and smooth contours not at all;
- `w_j` is 1 up to `R0` and falls smoothly to 0 at `R1`, so a corner entering or leaving the
  neighborhood does not switch `t` abruptly.

`R0` is the larger of the perimeter's line width and the largest configured nozzle diameter: the
measured angle of a vertex reflects geometry within at least one nozzle diameter, so a candidate
inserted next to a corner usually still sees that corner. Within `R0` of a corner of at least
`A1`, `t` and the target are the same as with a fixed minimum of 0.4.

### Placing the target on the loop

The target is generally off the loop. Its closest loop point is a polygon vertex over a whole range
of target positions, so it snaps to vertices and steps between layers when the vertices differ from
layer to layer.

Instead the loop gets a continuous normal field. Each vertex has the normalized sum of the normals
of its two edges, taken to the nearest points at a different position, so that duplicated points
(shared ends of extrusion paths, a repeated closing point) get one normal. Along an edge the normal
blends linearly between its vertex normals. A **solution** is a loop point whose normal line passes
through the target; on each edge this is a quadratic equation in the edge parameter.

The seam is the solution closest to the target over the whole loop. If it lies outside the allowed
arc, the seam is the end of that arc closer to the solution, so a solution leaving the arc stays at
the end it leaves through. Without any solution, which takes a degenerate loop, the seam is C.

The search walks outward from C on both sides, the shorter walked side first. An edge farther from
the target than the best solution so far is skipped without solving.

Properties:

- On a regular polygon every normal line passes through the center. For a target other than the
  center, the closest solution is the nearer of the two intersections of the loop with the line
  through the center and the target, wherever the vertices are; the seam equals it when it lies
  inside the allowed arc. With an even number of vertices the two intersections are symmetric about
  the center, so the nearer one is on the ray from the center through the target.
- When the closest solution is unique and lies inside the allowed arc, the result depends on the
  target and that arc only, not on which candidate inside the arc was chosen. A candidate step
  therefore does not pass into the seam directly. At an exact distance tie, the first solution found
  from C is kept.
- A selected solution varies continuously while its root branch remains nondegenerate, and so does
  the seam when the solution leaves or enters the allowed arc. The seam may jump when the closest
  solution changes (two solutions equally close, or the closest one disappearing together with
  another where the target crosses the curve of centers of the normal field), or when a solution
  outside the arc becomes equally close to both arc ends. Changes of the candidate types between
  layers change the allowed arc.

### Allowed arc

- For an Enforced C on a loop with exact ends: the Enforced run itself, which ends at the paint.
- For another Enforced C: the Enforced run, extended past each open end by the oversampling distance
  (0.2 mm) but never into a Blocked candidate. The run ends sit on candidates and move in sampling
  steps between layers, and Rear tends to choose the candidate right at an end; without the
  extension a target just past an end would copy those steps into the seam.
- For another C: the part of the loop between the Blocked candidates around it.
- For a Blocked C, or a loop with no limiting candidate: the whole loop.

## Numerical constants

| Constant | Value | Meaning |
| --- | --- | --- |
| Blend minimum near corners | 0.4 | Pull toward C on a painted corner |
| `A0`, `A1` | 15°, 30° | Measured angle where corner contribution starts and becomes full |
| `R0` | max(line width, largest nozzle diameter); 0.5 mm if both are zero | Full corner protection along the loop |
| `R1` | 2·`R0` | Corner influence ends |
| Run extension | 0.2 mm (`enforcer_oversampling_distance`); 0 with exact ends | How far the seam may go past an end of the Enforced run |
| Painted range tolerance | 1 µm (`enforced_end_tolerance`) | Loop positions this close are one point |
| Range end reach | line width | How far from the loop a contour crossing may lie |
| Coverage proof depth | 8 bisections per edge | Finite conservative proof for complete paint on a partly painted object |
| Solver threshold | 1e-12 | Absolute threshold on the equation coefficients that tells degenerate equations apart; not a distance tolerance |

## Constraints

Strong Precise Seam coordinates remain immutable. Angles of at least 55 degrees retain the
selected candidate. Ambiguous paint boundaries use the proximity fallback; an entirely
blocked perimeter still needs a printed seam. Mesh topology and candidate sampling can change
which first-approximation string is selected. Painting a verified complete perimeter introduces
no artificial center preference.

## Implementation and verification

`SeamPlacer.cpp` implements paint reconstruction, candidate selection, string traversal, fitting
and contour placement. `test_seam_placer.cpp` covers geometry, paint, string traversal and strong
Precise Seam invariants. `tests/libslic3r/test_seam_contour.cpp` independently checks
geometric correspondence, ambiguity and search-radius behavior.
