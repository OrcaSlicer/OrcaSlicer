#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../Polyline.hpp"

namespace Slic3r {

// Orca: NaN or infinite factors disable the smoothing, everything else is clamped to <0, 1>.
inline double sanitize_smooth_factor(double smooth_factor)
{
    return std::isfinite(smooth_factor) ? std::clamp(smooth_factor, 0., 1.) : 0.;
}

// Orca: Replaces the sharp vertices of an infill path with curves that join the adjoining straight
// legs with a continuous curvature, so the toolhead does not have to stop in every corner.
// Points are pushed one by one, because the plane path fills produce their path on the fly, and
// every point of the smoothed path is handed over to the caller supplied emit callback.
// Fully smoothed adjacent corners meet at the midpoint of the segment they share, so the emitted
// points may collapse onto each other once rounded to the integer grid of the caller. Dropping such
// duplicates is left to the caller, which is the only one knowing that grid.
class CornerSmoother
{
public:
    // tolerance is the maximum chordal deviation of the flattened curves, in the units of the pushed points.
    CornerSmoother(double smooth_factor, double tolerance)
        : m_corner_distance_ratio(0.5 * sanitize_smooth_factor(smooth_factor)), m_tolerance(tolerance)
    {}

    bool enabled() const { return m_corner_distance_ratio > 0.; }

    template<typename Emit> void push(const Vec2d &point, Emit &emit)
    {
        if (m_pending == 0) {
            emit(point);
            m_previous = point;
        } else if (m_pending > 1) {
            round_corner(m_previous, m_corner, point);
            for (const Vec2d &corner_point : m_corner_points)
                emit(corner_point);
            m_previous = m_corner;
        }
        m_corner  = point;
        m_pending = std::min(m_pending + 1, 2);
    }

    // Emits the last point of the path and prepares the smoother for a new one.
    template<typename Emit> void flush(Emit &emit)
    {
        if (m_pending > 1)
            emit(m_corner);
        m_pending = 0;
    }

private:
    // Fills m_corner_points with the points replacing the corner vertex.
    void round_corner(const Vec2d &previous, const Vec2d &corner, const Vec2d &next);
    // Flattens the canonical corner curve of the given size and turn into coordinates of the
    // (incoming, outgoing) basis of the corner. Cached, as an infill path repeats the same corner.
    const std::vector<Vec2d>& curve_coefficients(double corner_distance, const Vec2d &incoming, const Vec2d &outgoing);

    // Fraction of the shorter adjoining segment consumed on each side of a corner. Half of a segment
    // is the maximum, otherwise the curves of two adjacent corners would overlap.
    const double       m_corner_distance_ratio;
    const double       m_tolerance;
    std::vector<Vec2d> m_corner_points;
    // Cached flattening of the last corner, valid for corners of the same size and turn angle.
    std::vector<Vec2d> m_cached_coefficients;
    double             m_cached_distance { 0. };
    double             m_cached_cosine { 0. };
    bool               m_has_cached_coefficients { false };

    Vec2d m_previous { Vec2d::Zero() };
    Vec2d m_corner { Vec2d::Zero() };
    // Number of points held back: none, the first point of a path, or a corner candidate.
    int   m_pending { 0 };
};

// Rounds the corners of already scaled paths in place. Paths of less than three points are left alone.
void smooth_polyline_corners(Polyline &polyline, double smooth_factor, double tolerance);
void smooth_polylines_corners(Polylines &polylines, double smooth_factor, double tolerance);

} // namespace Slic3r
