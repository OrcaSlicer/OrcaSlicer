#pragma once

#include "PreciseSeam.hpp"
#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/libslic3r.h"
#include <cstddef>
#include <optional>
#include <vector>

namespace Slic3r::PreciseSeam::detail {

// Binding intermediates retain source edge identity until segment assembly.
struct ClippedEdgeInterval {
    size_t edge;
    double begin;
    double end;
    Point first;
    Point last;
    // The points of the reconstructed surface that `first` and `last` were carried from, on the surface path.
    std::optional<Point> surface_first;
    std::optional<Point> surface_last;
};

// Source of one edge of the surface line: a source edge shifted towards air, or the arc (convex) or
// bridge (unresolved concave) around a source vertex. Both map to their vertex.
struct SurfaceElement {
    size_t source; // Source edge or vertex index.
    bool vertex;
    // Parameters on the source edge where this part of a shifted edge starts and ends: a shifted edge is split
    // where its shift changes along it, and ends at the crossing of a concave corner instead of the shifted vertex.
    double begin = 0.;
    double end = 1.;
};

// Where coverage of the surface ends on one side of a dead stretch of the centerline (around a resolved
// concave corner) and resumes on the other. Both points are the crossing of the shifted edges, so coverage
// of both covers the stretch.
struct SurfaceGap {
    size_t end_element;
    Point end;
    size_t start_element;
    Point start;
};

// The reconstructed surface of one perimeter: the centerline shifted towards air, as a closed line with the
// source of every edge.
struct SurfaceLine {
    Polygon polygon;                      // No consecutive or closing duplicates.
    std::vector<SurfaceElement> elements; // One per polygon edge.
    // Integer shift of every source edge whose shift is constant along it: its shifted edge is an exact translate,
    // so a point maps back by subtraction. Other edges map back through the parameter.
    std::vector<Point> shifts;
    std::vector<bool> translated;         // Per source edge: whether `shifts` applies.
    Polyline line;                        // The polygon as an open path closed at its start, for clipping.
    std::vector<SurfaceGap> gaps;
    BoundingBox bounds;                   // Of the polygon.
};

// Builds the surface line of a perimeter accepted by PreparedPerimeter (either direction), shift > 0.
SurfaceLine build_surface_line(const Polygon &perimeter, coord_t shift, bool hole);
// The same with a shift that changes along the perimeter; no shift is negative.
SurfaceLine build_surface_line(const Polygon &perimeter, const ShiftProfile &profile, bool hole);

// A position on the centerline with its point.
struct CenterlinePoint {
    size_t edge;
    double parameter;
    Point point;
};

// Appends intervals covering the perimeter forward from `from` to `to`. Refuses to go around the whole loop:
// `to` before `from` on one edge appends nothing.
void cover_centerline(const Polygon &perimeter, CenterlinePoint from, const CenterlinePoint &to,
                      std::vector<ClippedEdgeInterval> &intervals);

// How a point of the smooth centerline of a fuzzy skin loop was carried onto its noisy loop.
enum class TransferOutcome {
    Exact,       // The crossing of the line through the point and its surface point nearest to the point.
    Approximate, // The pair coincides or the line crosses nothing within the window: the nearest point of the loop.
    Failed,      // Nothing of the noisy loop within the window.
};

struct Transfer {
    TransferOutcome outcome;
    CenterlinePoint position; // On the noisy loop; unset when the transfer failed.
};

// Carries `point` onto the noisy loop whose edges `target` indexes (line i is edge i), within `window` of it. An edge
// lying along the line gives its point nearest to `point`. Every edge must have non-zero length, as
// PreparedPerimeter::set_target() requires of the loop.
Transfer transfer_point(const AABBTreeLines::LinesDistancer<Line> &target, const Point &point, const Point &surface,
                        double window);

struct FragmentBindingFailure {
    size_t pair_index = 0;
    const char *reason = "empty fragment";
};

// Failure rolls back this fragment only; earlier bindings remain intact.
bool append_projected_fragment(const Polyline &fragment, const Polygon &perimeter,
                               std::vector<ClippedEdgeInterval> &intervals,
                               FragmentBindingFailure &failure);

// Exact path, then projection path, without the fallback; intervals are unchanged on failure.
// Exposed so tests can show that a fragment needs the fallback in append_fragment().
bool bind_fragment(const Polyline &fragment, const Polygon &perimeter,
                   std::vector<ClippedEdgeInterval> &intervals, FragmentBindingFailure &failure);

// Binds one fragment. After a failure it tries the rare-case repair and the contact rule, both logged
// as recoveries. Returns false when the fragment is discarded.
bool append_fragment(const Polyline &fragment, const Polygon &perimeter,
                     std::vector<ClippedEdgeInterval> &intervals,
                     const ExtractionContext &context, size_t fragment_index);

} // namespace Slic3r::PreciseSeam::detail
