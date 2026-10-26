# Fin support: Simple

[Fin support overview](../fin-support.md)

Simple chooses sites on sliced model contours authorized by paint. Its placement state is private to the selector; physical generation receives a complete candidate batch.

## Placement

Simple preprocesses painted side cuts into intervals on the actual sliced outer contours. The paint grants permission but never supplies candidate coordinates; intervals on the same model edge are merged. Consecutive painted edges on one island form a contact line while their turn stays within 15 degrees and the straight contact remains within half a support line width of that line. Candidate centers need enough room from the ends of the joined line for the complete contact bead, including its line width. On each highest available layer it ranks sharp convex corners and long straight-edge midpoints derived from the complete sliced model contour after scale-dependent simplification. A remaining interval midpoint is the fallback. Every chosen site subtracts an unavailable range at its own contour location and along its downward wall projection. If a lower section has no new wall intersection, the last wall location snaps to a same-facing contour within one support line width; that snapped location continues to the next section. The configured placement distance limits travel along the connected island contour at each location. Walking stops at a back-facing edge, so nearby edges across a fold or on another island remain available. Candidate selection exhausts the region before constructing fins. Each selected site becomes a standard fin candidate; tracing, periodic contact validation, projection, body geometry, finalization and toolpath generation then use the same pipeline as the other styles. Risk is fixed to one and only the existing surface-tilt falloff scales body expansion.

## Data and code boundaries

`SimpleCandidateLayer` owns the contour intervals and remaining selection space. `select_simple_fin_candidates()` chooses sites, subtracts unavailable contour/projection ranges and returns `std::vector<FinCandidate>`. It does not construct a connected Painted Shape intent graph.

`generate_candidate_fin_support()` consumes that batch. Simple uses the projected geometry mode, direct-paint contact authorization, per-fin finalization and overlap rejection. Failed candidates are excluded; they do not trigger the Simulated upward replanning loop.

The [shared sampling and settings](../fin-support.md#coordinate-and-sampling-conventions) define slice heights, placement distance and expansion. [Physical generation](generation.md) specifies painted contact coverage, body checks, taper, outlines and merged bed feet. [Eligibility and failure scope](generation.md#eligibility-and-failure-scope) records where a candidate or band is rejected.
