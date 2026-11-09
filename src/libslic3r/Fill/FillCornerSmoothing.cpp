#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <vector>
#include <utility>
#include <cmath>
#include <cstdlib>

#include "FillCornerSmoothing.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {

// Turns sharper than this are left untouched: both ends of the curve replacing such a corner nearly
// coincide, so the corner would be rounded into a degenerate loop instead of a hairpin.
static constexpr const double min_smoothed_turn_cosine = -0.9;

// Spacing of the control points of the corner curve along each leg, as a fraction of the reach.
static constexpr const double control_spacing = 0.3;

// Bounds the vertices of a corner whatever the tolerance.
static constexpr const size_t max_corner_vertices = 256;
// Bounds the corners a smoother keeps, for paths turning by ever new angles.
static constexpr const size_t max_cached_corners = 4096;

// The control points of the quintic Bezier corner curve.
using ControlPoints = std::array<Vec2d, 6>;

// The control points of the corner curve in a frame where the incoming leg runs along +x into the corner at
// the origin and the outgoing leg leaves it turning left. At each end, the first three control points are
// collinear and equally spaced: the tangent follows the adjoining leg and the second derivative is zero,
// so the curvature starts from zero and the toolhead eases into the turn.
static ControlPoints bezier_control_points(const double reach, const Vec2d &outgoing)
{
    const double spacing = control_spacing * reach;
    return { Vec2d(-reach, 0.), Vec2d(-(reach - spacing), 0.), Vec2d(-(reach - 2. * spacing), 0.),
             (reach - 2. * spacing) * outgoing, (reach - spacing) * outgoing, reach * outgoing };
}

// The first and second derivatives of the curve, by de Casteljau down to its last three points.
static std::pair<Vec2d, Vec2d> bezier_derivatives(const ControlPoints &control, const double t)
{
    ControlPoints work = control;
    for (size_t count = work.size(); count > 3; --count)
        for (size_t i = 0; i + 1 < count; ++i)
            work[i] = (1. - t) * work[i] + t * work[i + 1];
    const double degree = double(work.size() - 1);
    return { degree * ((1. - t) * (work[1] - work[0]) + t * (work[2] - work[1])),
             degree * (degree - 1.) * (work[2] - 2. * work[1] + work[0]) };
}

static Vec2d bezier_point(const ControlPoints &control, const double t)
{
    ControlPoints work = control;
    for (size_t count = work.size(); count > 1; --count)
        for (size_t i = 0; i + 1 < count; ++i)
            work[i] = (1. - t) * work[i] + t * work[i + 1];
    return work.front();
}

// A rounded corner of unit reach: its vertices in the (incoming, outgoing) basis of the corner, relative to it,
// and how far they stray from the curve.
struct UnitCorner
{
    std::vector<Vec2d> vertices;
    double             deviation { 0. };
};

// The corner shapes built on a thread. They depend on the turn and the vertex count only, so all the smoothers of
// the thread share them: some fills start a smoother for every path, and every layer starts its own.
struct CornerShapes
{
    std::map<std::pair<double, size_t>, UnitCorner> corners;
    std::map<double, double>                        peak_curvatures;
};
static thread_local CornerShapes corner_shapes;

// The peak curvature of the corner curve times its reach, which depends on the turn only.
static double peak_curvature_by_reach(const double cosine)
{
    std::map<double, double> &cache = corner_shapes.peak_curvatures;
    if (const auto it = cache.find(cosine); it != cache.end())
        return it->second;

    const double                  turn    = std::acos(std::clamp(cosine, -1., 1.));
    const ControlPoints           control = bezier_control_points(1., Vec2d(cosine, std::sin(turn)));
    static constexpr const size_t samples = 256;
    double                        peak    = 0.;
    for (size_t i = 0; i <= samples; ++i) {
        const auto [first, second] = bezier_derivatives(control, double(i) / double(samples));
        const double speed = first.norm();
        if (speed > EPSILON)
            peak = std::max(peak, std::abs(cross2(first, second)) / (speed * speed * speed));
    }
    if (cache.size() >= max_cached_corners)
        cache.clear();
    cache.emplace(cosine, peak);
    return peak;
}

// The polygon of `count` vertices of the corner of unit reach, and how far it strays from the curve.
static double tangent_polygon(const double cosine, const size_t count, std::vector<Vec2d> &vertices)
{
    const double turn     = std::acos(std::clamp(cosine, -1., 1.));
    const double sine     = std::sin(turn);
    const Vec2d  outgoing(cosine, sine);

    // The lines tangent to the curve at equal steps of direction, both legs included, and halfway between them.
    // Each vertex of the rounded corner is where two consecutive steps cross, so each turns by the same step.
    std::vector<std::pair<Vec2d, Vec2d>> tangents;
    tangents.reserve(2 * count + 1);
    tangents.emplace_back(Vec2d(-1., 0.), Vec2d(1., 0.));
    const ControlPoints control = bezier_control_points(1., outgoing);
    double              from    = 0.;
    for (size_t i = 1; i < 2 * count; ++i) {
        // The direction of the tangent grows monotonically along the curve, so Newton steps on it converge
        // within the bracket of the parameter; a step leaving the bracket is replaced by a bisection.
        const double angle = 0.5 * turn * double(i) / double(count);
        double       low = from, high = 1., t = 0.5 * (low + high);
        for (int iteration = 0; iteration < 64 && high - low > 1e-15; ++iteration) {
            const auto [first, second] = bezier_derivatives(control, t);
            const double error = std::atan2(first.y(), first.x()) - angle;
            if (std::abs(error) < 1e-14)
                break;
            if (error < 0.)
                low = t;
            else
                high = t;
            const double rate = cross2(first, second) / first.squaredNorm();
            const double next = rate > 0. ? t - error / rate : low;
            t = next > low && next < high ? next : 0.5 * (low + high);
        }
        from = low;
        tangents.emplace_back(bezier_point(control, t), bezier_derivatives(control, t).first.normalized());
    }
    tangents.emplace_back(outgoing, outgoing);

    vertices.clear();
    vertices.reserve(count);
    double deviation = 0.;
    for (size_t i = 0; i < count; ++i) {
        const auto &[point, direction]           = tangents[2 * i];
        const auto &[next_point, next_direction] = tangents[2 * i + 2];
        const Vec2d vertex = point + (cross2(next_point - point, next_direction) / cross2(direction, next_direction)) * direction;
        // The curve passes closest to the vertex about where it heads halfway between the two tangents.
        deviation = std::max(deviation, (vertex - tangents[2 * i + 1].first).norm());
        // In the (incoming, outgoing) basis, which is not orthonormal for turns other than a right angle.
        const double along_outgoing = vertex.y() / sine;
        vertices.emplace_back(vertex.x() - along_outgoing * cosine, along_outgoing);
    }
    return deviation;
}

// Valid until the next corner of the thread is built.
static const UnitCorner &unit_corner(const double cosine, const size_t count)
{
    std::map<std::pair<double, size_t>, UnitCorner> &cache = corner_shapes.corners;
    const std::pair<double, size_t>                  key(cosine, count);
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    if (cache.size() >= max_cached_corners)
        cache.clear();
    UnitCorner corner;
    corner.deviation = tangent_polygon(cosine, count, corner.vertices);
    return cache.emplace(key, std::move(corner)).first->second;
}

const std::vector<Vec2d> *CornerSmoother::corner_vertices(const double reach, const double cosine) const
{
    const double tolerance = m_smoothing.tolerance > 0. && std::isfinite(m_smoothing.tolerance) ? m_smoothing.tolerance : EPSILON;
    const double turn      = std::acos(std::clamp(cosine, -1., 1.));
    const double curvature = peak_curvature_by_reach(cosine) / reach;
    // Simplifying the path to the tolerance drops a vertex standing out of the chord of its neighbours by less,
    // which the sharpest vertex of the curve does at finer steps than this.
    const size_t most  = size_t(std::min(double(max_corner_vertices), turn / std::sqrt(2. * curvature * tolerance)));
    // The count an arc of the peak curvature takes; the flatter parts of the curve may need more.
    size_t       count = std::min(most, size_t(std::ceil(turn / std::sqrt(8. * curvature * tolerance) - EPSILON)));
    if (count < 2)
        return nullptr;
    for (;;) {
        const double deviation = reach * unit_corner(cosine, count).deviation;
        if (deviation <= tolerance || count >= most)
            break;
        // The deviation falls about with the square of the count.
        count = std::min(most, std::max(count + 1, size_t(std::ceil(double(count) * std::sqrt(deviation / tolerance)))));
    }
    return &unit_corner(cosine, count).vertices;
}

bool CornerSmoother::is_on_straight_run(const Vec2d &previous, const Vec2d &vertex, const Vec2d &next)
{
    const Vec2d  incoming_leg    = vertex - previous;
    const Vec2d  outgoing_leg    = next - vertex;
    const double incoming_length = incoming_leg.norm();
    const double outgoing_length = outgoing_leg.norm();
    // A vertex repeating one of its neighbours carries no direction of its own.
    if (incoming_length < EPSILON || outgoing_length < EPSILON)
        return true;

    const Vec2d incoming = incoming_leg / incoming_length;
    const Vec2d outgoing = outgoing_leg / outgoing_length;
    return incoming.dot(outgoing) > 0. &&
           std::abs(incoming.x() * outgoing.y() - incoming.y() * outgoing.x()) < EPSILON;
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
    // Snapped, so that the corners of one turn share the cached shapes whatever rounding their legs carry.
    const double cosine   = std::round(incoming.dot(outgoing) * 1e12) * 1e-12;
    // A collinear vertex is no corner at all, and a hairpin cannot be rounded, see above.
    if (std::abs(cross) < EPSILON || cosine < min_smoothed_turn_cosine) {
        m_corner_points.emplace_back(corner);
        return;
    }

    // Consuming at most half of the shorter leg keeps the curves of two adjacent corners apart.
    double reach = m_corner_distance_ratio * std::min(incoming_length, outgoing_length);
    if (m_max_corner_distance > 0.)
        reach = std::min(reach, m_max_corner_distance);

    const std::vector<Vec2d> *vertices = corner_vertices(reach, cosine);
    if (vertices == nullptr || (m_corner_filter && !m_corner_filter(corner - reach * incoming, corner + reach * outgoing))) {
        m_corner_points.emplace_back(corner);
        return;
    }

    m_corner_points.reserve(vertices->size());
    for (const Vec2d &vertex : *vertices)
        m_corner_points.emplace_back(corner + reach * (vertex.x() * incoming + vertex.y() * outgoing));
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

void smooth_polyline_corners(Polyline &polyline, const CornerSmoothing &smoothing, const double max_corner_distance,
                             const CornerFilter &corner_filter)
{
    CornerSmoother smoother(smoothing, max_corner_distance, corner_filter);
    if (!smoother.enabled() || polyline.size() < 3)
        return;

    polyline.points = smooth_corners(polyline.points, false, smoother);
    // Rounding back to the integer grid may collapse neighbouring vertices of a corner.
    polyline.remove_duplicate_points();
}

void smooth_polylines_corners(Polylines &polylines, const CornerSmoothing &smoothing, const double max_corner_distance,
                              const CornerFilter &corner_filter)
{
    if (sanitize_smooth_factor(smoothing.factor) == 0.)
        return;
    for (Polyline &polyline : polylines)
        smooth_polyline_corners(polyline, smoothing, max_corner_distance, corner_filter);
}

void smooth_polygons_corners(Polygons &polygons, const CornerSmoothing &smoothing, const double max_corner_distance,
                             const CornerFilter &corner_filter)
{
    CornerSmoother smoother(smoothing, max_corner_distance, corner_filter);
    if (!smoother.enabled())
        return;

    for (Polygon &polygon : polygons) {
        if (polygon.size() < 3)
            continue;
        polygon.points = smooth_corners(polygon.points, true, smoother);
        polygon.remove_duplicate_points();
        // The vertices of the first and of the last corner may meet on the leg they share. A polygon closes
        // implicitly, so it must not repeat its first vertex at the end.
        if (polygon.points.size() > 1 && polygon.points.front() == polygon.points.back())
            polygon.points.pop_back();
    }
}

} // namespace Slic3r
