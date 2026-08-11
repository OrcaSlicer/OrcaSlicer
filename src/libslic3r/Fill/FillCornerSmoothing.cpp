#include <array>

#include "FillCornerSmoothing.hpp"

namespace Slic3r {

// Turns sharper than this are left untouched: both ends of the curve replacing such a corner nearly
// coincide, so the corner would be rounded into a degenerate loop instead of a hairpin.
static constexpr const double min_smoothed_turn_cosine = -0.9;

// The control points are expressed in the (incoming, outgoing) basis of the corner, which is not
// orthonormal for turns other than a right angle.
using QuinticBezier = std::array<Vec2d, 6>;

static bool is_bezier_flat(const QuinticBezier &curve, const Vec2d &incoming, const Vec2d &outgoing, const double deviation)
{
    // A Bezier curve stays inside the convex hull of its control points. Therefore, keeping every
    // control point within a deviation-wide strip around the endpoint chord conservatively bounds the
    // flattening error. The cross product is the perpendicular distance scaled by the chord length;
    // comparing squared values avoids a square root.
    auto         in_plane        = [&incoming, &outgoing](const Vec2d &c) { return c.x() * incoming + c.y() * outgoing; };
    const Vec2d  chord           = in_plane(curve.back() - curve.front());
    const double chord_length_sq = chord.squaredNorm();
    const double max_cross_sq    = deviation * deviation * chord_length_sq;

    for (size_t i = 1; i + 1 < curve.size(); ++i) {
        const Vec2d  offset = in_plane(curve[i] - curve.front());
        const double cross  = chord.x() * offset.y() - chord.y() * offset.x();
        if (cross * cross > max_cross_sq)
            return false;
    }
    return true;
}

static void subdivide_bezier(const QuinticBezier &curve, QuinticBezier &left, QuinticBezier &right)
{
    // Split the curve at t = 0.5 using de Casteljau's algorithm. Each averaging level contributes one
    // control point to the left half and one to the right half; the latter is filled backwards to keep
    // both resulting control polygons in their original parameter direction.
    QuinticBezier subdivision = curve;
    left.front() = subdivision.front();
    right.back() = subdivision.back();
    for (size_t level = 1; level < curve.size(); ++level) {
        for (size_t i = 0; i + level < curve.size(); ++i)
            subdivision[i] = 0.5 * (subdivision[i] + subdivision[i + 1]);
        left[level] = subdivision.front();
        right[curve.size() - level - 1] = subdivision[curve.size() - level - 1];
    }
}

static void flatten_bezier(
    const QuinticBezier &curve, const Vec2d &incoming, const Vec2d &outgoing, const double deviation, std::vector<Vec2d> &output)
{
    // Subdivide to at least depth 1 so a rounded corner cannot collapse to a single diagonal chord.
    // A uniform subdivision depth keeps samples at equal parameter intervals t = k / 2^depth,
    // avoiding abrupt segment-length jumps at adaptive-depth boundaries.
    static constexpr size_t max_depth = 16;

    std::vector<QuinticBezier> subcurves(2);
    subdivide_bezier(curve, subcurves[0], subcurves[1]);

    for (size_t depth = 1; depth < max_depth; ++depth) {
        bool all_flat = true;
        for (const QuinticBezier &c : subcurves)
            if (!is_bezier_flat(c, incoming, outgoing, deviation)) {
                all_flat = false;
                break;
            }
        if (all_flat)
            break;
        std::vector<QuinticBezier> finer(subcurves.size() * 2);
        for (size_t i = 0; i < subcurves.size(); ++i)
            subdivide_bezier(subcurves[i], finer[i * 2], finer[i * 2 + 1]);
        subcurves = std::move(finer);
    }

    // The curve start is deliberately omitted so it can be shared with the straight leg feeding into it.
    output.clear();
    output.reserve(subcurves.size());
    for (const QuinticBezier &c : subcurves)
        output.emplace_back(c.back());
}

const std::vector<Vec2d>& CornerSmoother::curve_coefficients(
    const double corner_distance, const Vec2d &incoming, const Vec2d &outgoing)
{
    const double cosine = incoming.dot(outgoing);
    // Corners of the same size and turn angle are congruent, so they flatten identically. An infill
    // path walks over the very same corner over and over again, the Hilbert curve over a single one.
    if (m_has_cached_coefficients && corner_distance == m_cached_distance && cosine == m_cached_cosine)
        return m_cached_coefficients;

    // One canonical corner running from -corner_distance along the incoming leg to corner_distance
    // along the outgoing one. At each end, the first three control points are collinear and equally
    // spaced: the tangent follows the adjoining straight leg and the second derivative is zero. The
    // endpoint curvature is therefore zero, giving G2 joins to both legs.
    const double        d = corner_distance;
    const QuinticBezier corner_curve {{
        {-d, 0.}, {-0.7 * d, 0.}, {-0.4 * d, 0.}, {0., 0.4 * d}, {0., 0.7 * d}, {0., d}
    }};
    // Retain a finite positive tolerance if the smoother was set up with an invalid one.
    const double deviation = m_tolerance > 0. && std::isfinite(m_tolerance) ? m_tolerance : EPSILON;
    flatten_bezier(corner_curve, incoming, outgoing, deviation, m_cached_coefficients);

    m_cached_distance         = corner_distance;
    m_cached_cosine           = cosine;
    m_has_cached_coefficients = true;
    return m_cached_coefficients;
}

void CornerSmoother::round_corner(const Vec2d &previous, const Vec2d &corner, const Vec2d &next)
{
    m_corner_points.clear();

    const Vec2d  incoming_leg = corner - previous;
    const Vec2d  outgoing_leg = next - corner;
    const double incoming_length = incoming_leg.norm();
    const double outgoing_length = outgoing_leg.norm();
    if (incoming_length < EPSILON || outgoing_length < EPSILON) {
        m_corner_points.emplace_back(corner);
        return;
    }

    const Vec2d  incoming = incoming_leg / incoming_length;
    const Vec2d  outgoing = outgoing_leg / outgoing_length;
    const double cross    = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
    // A collinear vertex is no corner at all, and a hairpin cannot be rounded, see above.
    if (std::abs(cross) < EPSILON || incoming.dot(outgoing) < min_smoothed_turn_cosine) {
        m_corner_points.emplace_back(corner);
        return;
    }

    // Consuming at most half of the shorter leg keeps the curves of two adjacent corners apart.
    double corner_distance = m_corner_distance_ratio * std::min(incoming_length, outgoing_length);
    if (m_max_corner_distance > 0.)
        corner_distance = std::min(corner_distance, m_max_corner_distance);

    const Vec2d curve_start = corner - corner_distance * incoming;
    const Vec2d curve_end   = corner + corner_distance * outgoing;
    if (m_corner_filter && !m_corner_filter(curve_start, curve_end)) {
        m_corner_points.emplace_back(corner);
        return;
    }

    const std::vector<Vec2d> &coefficients = curve_coefficients(corner_distance, incoming, outgoing);
    m_corner_points.reserve(coefficients.size() + 1);
    m_corner_points.emplace_back(curve_start);
    for (const Vec2d &coefficient : coefficients)
        m_corner_points.emplace_back(corner + coefficient.x() * incoming + coefficient.y() * outgoing);
}

// Rounds the corners of a scaled point sequence. A polygon closes implicitly, so all of its vertices
// are corners; a polyline is an open path that keeps both of its ends, even where they coincide - a
// path returning to where it started retraces its way back and is not a loop.
static Points smooth_corners(const Points &points, const bool polygon, CornerSmoother &smoother)
{
    // A polygon has no free ends, so its first vertex is a corner like any other. Rounding it takes
    // feeding the smoother the last vertex first, whose own output point is then dropped again.
    size_t skip = polygon ? 1 : 0;

    Points smoothed;
    smoothed.reserve(2 * points.size());
    auto emit = [&smoothed, &skip](const Vec2d &point) {
        if (skip > 0) {
            --skip;
            return;
        }
        smoothed.emplace_back(coord_t(std::floor(point.x() + 0.5)), coord_t(std::floor(point.y() + 0.5)));
    };

    if (polygon)
        smoother.push(points.back().cast<double>(), emit);
    for (const Point &point : points)
        smoother.push(point.cast<double>(), emit);
    if (polygon)
        // Wrap the first vertex around, so that the last one is a corner as well.
        smoother.push(points.front().cast<double>(), emit);
    smoother.flush(emit);

    if (polygon)
        // The flushed point is the wrapped first vertex, which a polygon does not store.
        smoothed.pop_back();
    return smoothed;
}

void smooth_polyline_corners(Polyline &polyline, const double smooth_factor, const double tolerance,
                             const double max_corner_distance, const CornerFilter &corner_filter)
{
    CornerSmoother smoother(smooth_factor, tolerance, max_corner_distance, corner_filter);
    if (!smoother.enabled() || polyline.size() < 3)
        return;

    polyline.points = smooth_corners(polyline.points, false, smoother);
    // Rounding back to the integer grid may collapse neighbouring samples of a curve.
    polyline.remove_duplicate_points();
}

void smooth_polylines_corners(Polylines &polylines, const double smooth_factor, const double tolerance,
                              const double max_corner_distance, const CornerFilter &corner_filter)
{
    if (sanitize_smooth_factor(smooth_factor) == 0.)
        return;
    for (Polyline &polyline : polylines)
        smooth_polyline_corners(polyline, smooth_factor, tolerance, max_corner_distance, corner_filter);
}

void smooth_polygons_corners(Polygons &polygons, const double smooth_factor, const double tolerance,
                             const double max_corner_distance, const CornerFilter &corner_filter)
{
    CornerSmoother smoother(smooth_factor, tolerance, max_corner_distance, corner_filter);
    if (!smoother.enabled())
        return;

    for (Polygon &polygon : polygons) {
        if (polygon.size() < 3)
            continue;
        polygon.points = smooth_corners(polygon.points, true, smoother);
        polygon.remove_duplicate_points();
        // The curves of the first and of the last corner may have met on the segment they share. A
        // polygon closes implicitly, so it must not repeat its first vertex at the end.
        if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back())
            polygon.points.pop_back();
    }
}

} // namespace Slic3r
