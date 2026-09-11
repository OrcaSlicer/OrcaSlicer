#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include "../libslic3r.h"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"

namespace Slic3r {

// Orca: NaN or infinite factors disable the smoothing, everything else is clamped to <0, 1>.
inline double sanitize_smooth_factor(double smooth_factor)
{
    return std::isfinite(smooth_factor) ? std::clamp(smooth_factor, 0., 1.) : 0.;
}

// Decides whether a corner may be replaced by the curve that leaves the path at `from` and rejoins it
// at `to`, both in the coordinate system of the pushed points. Rounding cuts toward the inside of the
// turn, so a path that is not clipped to the fill region afterwards needs this to stay inside it.
using CornerFilter = std::function<bool(const Vec2d &from, const Vec2d &to)>;

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
    // tolerance is the maximum chordal deviation of the flattened curves, in the units of the pushed
    // points. max_corner_distance caps how far a curve may reach along a leg, in the same units; it
    // bounds how far a rounded corner moves away from the original path, which matters where the legs
    // are much longer than the spacing of the pattern. Zero leaves the reach uncapped.
    CornerSmoother(double smooth_factor, double tolerance, double max_corner_distance = 0.,
                   CornerFilter corner_filter = {})
        : m_corner_distance_ratio(0.5 * sanitize_smooth_factor(smooth_factor)), m_tolerance(tolerance),
          m_max_corner_distance(max_corner_distance), m_corner_filter(std::move(corner_filter))
    {}

    bool enabled() const { return m_corner_distance_ratio > 0.; }

    template<typename Emit> void push(const Vec2d &point, Emit &emit)
    {
        if (m_held == 0) {
            // The first point of a path is an end, not a corner, and stays where it is.
            emit(point);
            m_window[m_held++] = point;
            return;
        }
        if (m_held > 1 && is_on_straight_run(m_window[m_held - 2], m_window[m_held - 1], point)) {
            // The newest vertex only splits a straight leg, so the leg runs on to this point instead.
            m_window[m_held - 1] = point;
            return;
        }
        if (m_held < 3) {
            m_window[m_held++] = point;
            return;
        }
        // Both legs of the middle vertex are complete now, so its curve can no longer grow.
        emit_corner(m_window[0], m_window[1], m_window[2], emit);
        m_window[0] = m_window[1];
        m_window[1] = m_window[2];
        m_window[2] = point;
    }

    // Emits the last point of the path and prepares the smoother for a new one.
    template<typename Emit> void flush(Emit &emit)
    {
        if (m_held > 2)
            emit_corner(m_window[0], m_window[1], m_window[2], emit);
        if (m_held > 1)
            emit(m_window[m_held - 1]);
        m_held = 0;
    }

private:
    template<typename Emit> void emit_corner(const Vec2d &previous, const Vec2d &corner, const Vec2d &next, Emit &emit)
    {
        round_corner(previous, corner, next);
        for (const Vec2d &corner_point : m_corner_points)
            emit(corner_point);
    }

    // Tells a vertex that only continues a straight leg (or repeats its predecessor) from a corner.
    // A path doubling back on itself is not one, that vertex is a hairpin and stays where it is.
    static bool is_on_straight_run(const Vec2d &previous, const Vec2d &vertex, const Vec2d &next);
    // Fills m_corner_points with the points replacing the corner vertex.
    void round_corner(const Vec2d &previous, const Vec2d &corner, const Vec2d &next);
    // Flattens the canonical corner curve of the given size and turn into coordinates of the
    // (incoming, outgoing) basis of the corner. Cached, as an infill path repeats the same corner.
    const std::vector<Vec2d>& curve_coefficients(double corner_distance, const Vec2d &incoming, const Vec2d &outgoing);

    // Fraction of the shorter adjoining leg consumed on each side of a corner. Half of a leg is the
    // maximum, otherwise the curves of two adjacent corners would overlap.
    const double       m_corner_distance_ratio;
    const double       m_tolerance;
    const double       m_max_corner_distance;
    const CornerFilter m_corner_filter;
    std::vector<Vec2d> m_corner_points;
    // Cached flattening of the last corner, valid for corners of the same size and turn angle.
    std::vector<Vec2d> m_cached_coefficients;
    double             m_cached_distance { 0. };
    double             m_cached_cosine { 0. };
    bool               m_has_cached_coefficients { false };

    // The corners seen last, kept free of vertices that merely split a straight leg. The middle one
    // is rounded once the third arrives, which is what makes its outgoing leg final.
    std::array<Vec2d, 3> m_window { Vec2d::Zero(), Vec2d::Zero(), Vec2d::Zero() };
    // How many of them are filled in.
    int                  m_held { 0 };
};

// Rounds the corners of already scaled paths in place. Paths of less than three points are left alone.
// Both ends of a polyline are kept where they are, even when they coincide: such a path retraces its
// way back and joining its ends would turn it into a loop. See CornerSmoother for max_corner_distance.
void smooth_polyline_corners(Polyline &polyline, double smooth_factor, double tolerance,
                             double max_corner_distance = 0., const CornerFilter &corner_filter = {});
void smooth_polylines_corners(Polylines &polylines, double smooth_factor, double tolerance,
                              double max_corner_distance = 0., const CornerFilter &corner_filter = {});
// Polygons close implicitly, so every one of their vertices is a corner.
void smooth_polygons_corners(Polygons &polygons, double smooth_factor, double tolerance,
                             double max_corner_distance = 0., const CornerFilter &corner_filter = {});

} // namespace Slic3r
