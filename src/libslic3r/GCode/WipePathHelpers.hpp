#pragma once

#include <optional>

#include "../ExtrusionEntity.hpp"
#include "../Polyline.hpp"
#include "../Line.hpp"

namespace Slic3r {

// Orca: sample a point at a given distance along ExtrusionPaths, walking
// across segment boundaries. forward=true walks from paths.front, false from
// paths.back. For tiny loops the walk stops early and returns the last
// reachable point. Returns the start point if target is zero.
// Precondition: paths must be non-empty.
Point sample_path_at_distance(const ExtrusionPaths &paths, bool forward, double target);

// Orca: return the side of the printed path on which the material lies.
// dir +1 is left and -1 is right, matching the offset-builder convention.
int wipe_offset_direction(bool is_ccw, bool is_hole);

// Orca: atomically offset a stored wipe path. The seam-gap or closing edge
// determines the join with the first outgoing perimeter edge, but its offset
// is not part of the executable wipe. Only the prefix needed by Wipe::wipe()
// is offset. Returns false and leaves polyline unchanged if that path cannot
// be constructed without degenerate segments. The caller must use
// wipe_path_is_supported() before accepting the result. The first stored point
// remains a dummy preserving Wipe::wipe()'s convention of skipping points[0].
// Precondition: polyline starts at seam_start, dir is +1 or -1, and
// offset_dist > 0. A non-positive max_wipe_length returns false.
bool offset_wipe_path(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                      int dir, double offset_dist, double max_wipe_length);

// Orca: verify that the complete executable offset wipe path stays near the
// current or an earlier perimeter and that an earlier perimeter is locally
// available at the wipe start. The first stored point is the dummy skipped by
// Wipe::wipe(), so wipe_start is used as the actual start of the first segment.
bool wipe_path_is_supported(const Polyline &polyline, Point wipe_start,
                            const Lines &other_perimeter_lines, const Lines &current_perimeter_lines,
                            double max_distance);

// Orca: identify the adjacent inner perimeter from the outgoing wall, excluding
// support on the air side of a closed zero-gap loop. Clamp the requested offset
// to the distance from the seam end to that support, then select the safest
// supported offset or translated path. If a wide seam gap at a corner truncates
// every forward candidate, the incoming printed wall may be followed backwards
// instead. All earlier printed perimeters still participate in the complete-path
// safety check. This handles converging, locally ambiguous, or self-touching
// contours whose global winding alone does not identify the material side.
// Returns false and leaves polyline unchanged when no candidate is supported.
// Precondition: preferred_dir is +1 or -1. Distances must be positive.
bool offset_wipe_path_toward_support(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                                     int preferred_dir, double offset_dist, double max_wipe_length,
                                     const Lines &target_perimeter_lines, const Lines &printed_perimeter_lines,
                                     const Lines &current_perimeter_lines,
                                     double max_support_distance);

// Orca: compute the inward destination point for wipe_on_loops, or
// std::nullopt when the geometry is degenerate (tiny loop, coincident samples,
// angle near 0 or 2π). Returns the rotated destination or nullopt to skip the
// inward move entirely.
// Precondition: paths non-empty, nozzle_diam_scaled > 0.
std::optional<Point> wipe_on_loops_destination(const ExtrusionPaths &paths, double nozzle_diam_scaled,
                                                 bool is_ccw, bool is_hole);

} // namespace Slic3r
