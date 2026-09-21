#pragma once

#include <optional>

#include "../ExtrusionEntity.hpp"
#include "../Polyline.hpp"
#include "../Line.hpp"

namespace Slic3r {

// Printed prefix of one region's perimeter sequence. Append each entity only
// after extrusion; later walls and other regions cannot support an inward wipe.
struct WipeInwardSupport {
    Lines printed_lines;
    Lines inner_lines;
    void append(const ExtrusionEntity &entity);
};

namespace AABBTreeLines {
template <typename LineType> class LinesDistancer;
}

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
// be constructed without degenerate segments. This only constructs a candidate;
// offset_wipe_path_toward_support() validates its support, material side and
// connector before accepting it. The first stored point
// remains a dummy preserving Wipe::wipe()'s convention of skipping points[0].
// Precondition: polyline starts at seam_start, dir is +1 or -1, and
// offset_dist > 0. A non-positive max_wipe_length returns false.
bool offset_wipe_path(Polyline &polyline, Point seam_start, Point seam_end, Point wipe_start,
                      int dir, double offset_dist, double max_wipe_length);

// Orca: score a candidate's first destination by distance to the target inner
// walls. Return nullopt if no target wall is near wipe_start or any executable
// segment lacks support. target_distancer contains eligible earlier walls;
// all_support_distancer includes the current wall and all earlier walls.
// The stored first point is a dummy: the first segment starts at wipe_start.
// This checks support only; material-side and connector checks belong to
// offset_wipe_path_toward_support(). Trees are reused across its candidates.
std::optional<double> wipe_path_support_score(
    const Polyline &polyline, Point wipe_start,
    const AABBTreeLines::LinesDistancer<Line> &target_distancer,
    const AABBTreeLines::LinesDistancer<Line> &all_support_distancer,
    double max_distance);

// Validate the initial inward direction and the local material side along the
// executable path, using the inner wall to orient the open current wall's
// normals. Clearance is optional for clipped corners and short direct fallbacks;
// the material-side check is mandatory. The straight connector is checked by
// its initial direction and separately by support and intersection validation.
// path_start is the construction origin; points[0] is only a storage sentinel.
bool wipe_path_stays_on_material_side(
    const Polyline &path, Point path_start, const Vec2d &support_direction,
    const AABBTreeLines::LinesDistancer<Line> &target_perimeter_distancer,
    const AABBTreeLines::LinesDistancer<Line> &current_perimeter_distancer,
    double effective_offset, bool require_clearance);

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
