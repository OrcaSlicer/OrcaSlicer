// Polygon offsetting using Voronoi diagram prodiced by boost::polygon.

#include <cmath>
#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>
#include <cassert>
#include <cstdlib>

#include "libslic3r/Geometry.hpp"
#include "VoronoiOffset.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Geometry/Voronoi.hpp"

// #define VORONOI_DEBUG_OUT

#ifdef VORONOI_DEBUG_OUT
#include <libslic3r/VoronoiVisualUtils.hpp>
#endif

namespace Slic3r {
namespace Voronoi {

namespace detail {
    // Intersect a circle with a ray, return the two parameters.
    // Currently used for unbounded Voronoi edges only.
	double first_circle_segment_intersection_parameter(
		const Vec2d &center, const double r, const Vec2d &pt, const Vec2d &v)
	{
		const Vec2d 	d = pt - center;
#ifndef NDEBUG
        // Start point should be inside, end point should be outside the circle.
        double          d0 = (pt - center).norm();
        double          d1 = (pt + v - center).norm();
        assert(d0 < r + SCALED_EPSILON);
        assert(d1 > r - SCALED_EPSILON);
#endif /* NDEBUG */
        const double	a = v.squaredNorm();
		const double 	b = 2. * d.dot(v);
		const double    c = d.squaredNorm() - r * r;
		std::pair<int, std::array<double, 2>> out;
        double          u = b * b - 4. * a * c;
		assert(u > - EPSILON);
		double          t;
		if (u <= 0) {
			// Degenerate to a single closest point.
			t = - b / (2. * a);
			assert(t >= - EPSILON && t <= 1. + EPSILON);
			return std::clamp(t, 0., 1.);
		} else {
			u = sqrt(u);
			out.first = 2;
			double t0 = (- b - u) / (2. * a);
			double t1 = (- b + u) / (2. * a);
			// One of the intersections shall be found inside the segment.
			assert((t0 >= - EPSILON && t0 <= 1. + EPSILON) || (t1 >= - EPSILON && t1 <= 1. + EPSILON));
			if (t1 < 0.)
				return 0.;
			if (t0 > 1.)
				return 1.;
			return (t0 > 0.) ? t0 : t1;
		}
	}

    struct Intersections
    {
        int     count;
        Vec2d   pts[2];
    };

    // Return maximum two points, that are at distance "d" from both points
    Intersections point_point_equal_distance_points(const Point &pt1, const Point &pt2, const double d)
    {
        // Calculate the two intersection points.
        // With the help of Python package sympy:
        //      res = solve([(x - cx)**2 + (y - cy)**2 - d**2, x**2 + y**2 - d**2], [x, y])
        //      ccode(cse((res[0][0], res[0][1], res[1][0], res[1][1])))
        // where cx, cy is the center of pt1 relative to pt2,
        // d is distance from the line and the point (0, 0).
        // The result is then shifted to pt2.
        auto   cx = double(pt1.x() - pt2.x());
        auto   cy = double(pt1.y() - pt2.y());
        double cl = cx * cx + cy * cy;
        double discr = 4. * d * d - cl;
        if (discr < 0.) {
            // No intersection point found, the two circles are too far away.
            return Intersections { 0, { Vec2d(), Vec2d() } };
        }
        // Avoid division by zero if a gets too small.
        bool   xy_swapped = std::abs(cx) < std::abs(cy);
        if (xy_swapped)
            std::swap(cx, cy);
        double u;
        int    cnt;
        if (discr == 0.) {
            cnt = 1;
            u   = 0;
        } else {
            cnt = 2;
            u = 0.5 * cx * sqrt(cl * discr) / cl;
        }
        double v = 0.5 * cy - u;
        double w = 2.  * cy;
        double e = 0.5 / cx;
        double f = 0.5 * cy + u;
        Intersections out { cnt, { Vec2d(-e * (v * w - cl), v),
                                   Vec2d(-e * (w * f - cl), f) } };
        if (xy_swapped) {
            std::swap(out.pts[0].x(), out.pts[0].y());
            std::swap(out.pts[1].x(), out.pts[1].y());
        }
        out.pts[0] += pt2.cast<double>();
        out.pts[1] += pt2.cast<double>();

        assert(std::abs((out.pts[0] - pt1.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - pt1.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[0] - pt2.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - pt2.cast<double>()).norm() - d) < SCALED_EPSILON);
        return out;
    }

    // Return maximum two points, that are at distance "d" from both the line and point.
    Intersections line_point_equal_distance_points(const Line &line, const Point &ipt, const double d)
    {   
        assert(line.a != ipt && line.b != ipt);
        // Calculating two points of distance "d" to a ray and a point.
        // Point.
        Vec2d  pt   = ipt.cast<double>();
        Vec2d  lv   = (line.b - line.a).cast<double>();
        double l2   = lv.squaredNorm();
        Vec2d  lpv  = (line.a - ipt).cast<double>();
        double c    = cross2(lpv, lv);
        if (c < 0) {
            lv = - lv;
            c  = - c;
        }

        // Line equation (ax + by + c - d * sqrt(l2)).
        auto   a    = - lv.y();
        auto   b    = lv.x();
        // Line point shifted by -ipt is on the line.
        assert(std::abs(lpv.x() * a + lpv.y() * b + c) < SCALED_EPSILON);
        // Line vector (a, b) points towards ipt.
        assert(a * lpv.x() + b * lpv.y() < - SCALED_EPSILON);

#ifndef NDEBUG
        {
            // Foot point of ipt on line.
            Vec2d ft = Geometry::foot_pt(line, ipt);
            // Center point between ipt and line, its distance to both line and ipt is equal.
            Vec2d centerpt = 0.5 * (ft + pt) - pt;
            double dcenter = 0.5 * (ft - pt).norm();
            // Verify that the center point
            assert(std::abs(centerpt.x() * a + centerpt.y() * b + c - dcenter * sqrt(l2)) < SCALED_EPSILON * sqrt(l2));
        }
#endif // NDEBUG

        // Calculate the two intersection points.
        // With the help of Python package sympy:
        //      res = solve([a * x + b * y + c - d * sqrt(a**2 + b**2), x**2 + y**2 - d**2], [x, y])
        //      ccode(cse((res[0][0], res[0][1], res[1][0], res[1][1])))
        // where (a, b, c, d) is the line equation, not normalized (vector a,b is not normalized),
        // d is distance from the line and the point (0, 0).
        // The result is then shifted to ipt.

        double dscaled = d * sqrt(l2);
        double s       = c * (2. * dscaled - c);
        if (s < 0.)
            // Distance of pt from line is bigger than 2 * d.
            return Intersections { 0 };
        double u;
        int    cnt;
        // Avoid division by zero if a gets too small.
        bool   xy_swapped = std::abs(a) < std::abs(b);
        if (xy_swapped)
            std::swap(a, b);
        if (s == 0.) {
            // Distance of pt from line is 2 * d.
            cnt = 1;
            u   = 0.;
        } else {
            // Distance of pt from line is smaller than 2 * d.
            cnt = 2;
            u   = a * sqrt(s) / l2;
        }
        double e = dscaled - c;
        double f = b * e / l2;
        double g = f - u;
        double h = f + u;
        Intersections out { cnt, { Vec2d((- b * g + e) / a, g),
                                   Vec2d((- b * h + e) / a, h) } };
        if (xy_swapped) {
            std::swap(out.pts[0].x(), out.pts[0].y());
            std::swap(out.pts[1].x(), out.pts[1].y());
        }
        out.pts[0] += pt;
        out.pts[1] += pt;

        assert(std::abs(Geometry::ray_point_distance<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), out.pts[0]) - d) < SCALED_EPSILON);
        assert(std::abs(Geometry::ray_point_distance<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), out.pts[1]) - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[0] - ipt.cast<double>()).norm() - d) < SCALED_EPSILON);
        assert(std::abs((out.pts[1] - ipt.cast<double>()).norm() - d) < SCALED_EPSILON);
        return out;
    }

    // Double vertex equal to a coord_t point after conversion to double.
    template<typename VertexType>
    inline bool vertex_equal_to_point(const VertexType &vertex, const Point &ipt)
    {
        // Convert ipt to doubles, force the 80bit FPU temporary to 64bit and then compare.
        // This should work with any settings of math compiler switches and the C++ compiler
        // shall understand the memcpies as type punning and it shall optimize them out.
#if 1
        using ulp_cmp_type = boost::polygon::detail::ulp_comparison<double>;
        ulp_cmp_type ulp_cmp;
        static constexpr int ULPS = boost::polygon::voronoi_diagram_traits<double>::vertex_equality_predicate_type::ULPS;
        return ulp_cmp(vertex.x(), double(ipt.x()), ULPS) == ulp_cmp_type::EQUAL &&
               ulp_cmp(vertex.y(), double(ipt.y()), ULPS) == ulp_cmp_type::EQUAL;
#else
        volatile double u = static_cast<double>(ipt.x());
        volatile double v = vertex.x();
        if (u != v)
            return false;
        u = static_cast<double>(ipt.y());
        v = vertex.y();
        return u == v;
#endif
    };
    bool vertex_equal_to_point(const VD::vertex_type *vertex, const Point &ipt)
        { return vertex_equal_to_point(*vertex, ipt); }

    double dist_to_site(const Lines &lines, const VD::cell_type &cell, const Vec2d &point)
    {
        const Line &line = lines[cell.source_index()];
        return cell.contains_point() ?
            (((cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line.a : line.b).cast<double>() - point).norm() :
            (Geometry::foot_pt<Vec2d>(line.a.cast<double>(), (line.b - line.a).cast<double>(), point) - point).norm();
    };

    bool on_site(const Lines &lines, const VD::cell_type &cell, const Vec2d &pt)
    {
        const Line &line = lines[cell.source_index()];
        auto on_contour = [&pt](const Point &ipt) { return detail::vertex_equal_to_point(pt, ipt); };
        if (cell.contains_point()) {
            return on_contour(contour_point(cell, line));
        } else {
            assert(! (on_contour(line.a) && on_contour(line.b)));
            return on_contour(line.a) || on_contour(line.b);
        }
    };

    // For a Voronoi segment starting with voronoi_point1 and ending with voronoi_point2,
    // defined by a bisector of Voronoi sites pt1_site and pt2_site (line)
    // find two points on the Voronoi bisector, that delimit regions with dr/dl measure
    // lower / higher than threshold_dr_dl.
    //
    // Linear segment from voronoi_point1 to return.first and
    // linear segment from return.second to voronoi_point2 have dr/dl measure
    // higher than threshold_dr_dl.
    // If such respective segment does not exist, then return.first resp. return.second is nan.
    std::pair<Vec2d, Vec2d> point_point_dr_dl_thresholds(
        // Two Voronoi sites
        const Point &pt1_site, const Point &pt2_site,
        // End points of a Voronoi segment
        const Vec2d &voronoi_point1, const Vec2d &voronoi_point2,
        // Threshold of the skeleton function, where alpha is an angle of a sharp convex corner with the same dr/dl.
        const double threshold_tan_alpha_half)
    {
        // sympy code to calculate +-x
        // of a linear bisector of pt1_site, pt2_site parametrized with pt + x * v, |v| = 1
        // where dr/dl = threshold_dr_dl
        // equals d|pt1_site - pt + x * v| / dx = threshold_dr_dl
        //
        // y = sqrt(x^2 + d^2)
        // dy = diff(y, x)
        // solve(dy - c, x)
        //
        // Project voronoi_point1/2 to line_site.
        Vec2d  dir_y = (pt2_site - pt1_site).cast<double>();
        Vec2d  dir_x = Vec2d(- dir_y.y(), dir_y.x()).normalized();
        Vec2d  cntr  = 0.5 * (pt1_site.cast<double>() + pt2_site.cast<double>());
        double t1 = (voronoi_point1 - cntr).dot(dir_x);
        double t2 = (voronoi_point2 - cntr).dot(dir_x);
        if (t1 > t2) {
            t1 = -t1;
            t2 = -t2;
            dir_x = - dir_x;
        }
        auto x  = 0.5 * dir_y.norm() * threshold_tan_alpha_half;
        static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        auto out = std::make_pair(Vec2d(nan, nan), Vec2d(nan, nan));
        if (t2 > -x && t1 < x) {
            // Intervals overlap.
            dir_x *= x;
            out.first  = (t1 < -x) ? cntr - dir_x : voronoi_point1;
            out.second = (t2 > +x) ? cntr + dir_x : voronoi_point2;
        }
        return out;
    }

    // For a Voronoi segment starting with voronoi_point1 and ending with voronoi_point2,
    // defined by a bisector of Voronoi sites pt_site and line site (parabolic arc)
    // find two points on the Voronoi parabolic arc, that delimit regions with dr/dl measure
    // lower / higher than threshold_dr_dl.
    //
    // Parabolic arc from voronoi_point1 to return.first and
    // parabolic arc from return.second to voronoi_point2 have dr/dl measure
    // higher than threshold_dr_dl.
    // If such respective segment does not exist, then return.first resp. return.second is nan.
    std::pair<Vec2d, Vec2d> point_segment_dr_dl_thresholds(
        // Two Voronoi sites
        const Point &pt_site, const Line &line_site,
        // End points of a Voronoi segment
        const Vec2d &voronoi_point1, const Vec2d &voronoi_point2,
        // Threshold of the skeleton function, where alpha is an angle of a sharp convex corner with the same dr/dl.
        const double threshold_tan_alpha_half)
    {
        // sympy code to calculate  +-x
        // of a parabola            y = ax^2 + b
        // where                    dr/dl = threshold_dr_dl
        //
        // a = 1 / (4 * b)
        // y = a*x**2 + b
        // dy = diff(y, x)
        // solve(dy / sqrt(1 + dy**2) - c, x)
        //
        // Foot point of the point site on the line site.
        Vec2d  ft  = Geometry::foot_pt(line_site, pt_site);
        // Minimum distance of the bisector (parabolic arc) from the two sites, squared.
        Vec2d  dir_pt_ft = pt_site.cast<double>() - ft;
        double b   = 0.5 * dir_pt_ft.norm();
        static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        auto   out = std::make_pair(Vec2d(nan, nan), Vec2d(nan, nan));
        {
            // +x, -x are the two parameters along the line_site, where threshold_tan_alpha_half is met.
            double x  = 2. * b * threshold_tan_alpha_half;
            // Project voronoi_point1/2 to line_site.
            Vec2d  dir_x = (line_site.b - line_site.a).cast<double>().normalized();
            double t1 = (voronoi_point1 - ft).dot(dir_x);
            double t2 = (voronoi_point2 - ft).dot(dir_x);
            if (t1 > t2) {
                t1 = -t1;
                t2 = -t2;
                dir_x = - dir_x;
            }
            if (t2 > -x && t1 < x) {
                // Intervals overlap.
                bool t1_valid = t1 < -x;
                bool t2_valid = t2 > +x;
                // Direction of the Y axis of the parabola.
                Vec2d dir_y(- dir_x.y(), dir_x.x());
                // Orient the Y axis towards the point site.
                if (dir_y.dot(dir_pt_ft) < 0.)
                    dir_y = - dir_y;
                // Equation of the parabola: y = b + a * x^2
                double a = 0.25 / b;
                dir_x *= x;
                dir_y *= b + a * x * x;
                out.first  = t1_valid ? ft - dir_x + dir_y : voronoi_point1;
                out.second = t2_valid ? ft + dir_x + dir_y : voronoi_point2;
            }
        }
        return out;
    }

    std::pair<Vec2d, Vec2d> point_point_skeleton_thresholds(
        // Two Voronoi sites
        const Point &pt1_site, const Point &pt2_site,
        // End points of a Voronoi segment
        const Vec2d &voronoi_point1, const Vec2d &voronoi_point2,
        // Threshold of the skeleton function.
        const double tan_alpha_half)
    {
        // Project voronoi_point1/2 to line_site.
        Vec2d  dir_y = (pt2_site - pt1_site).cast<double>();
        Vec2d  dir_x = Vec2d(- dir_y.y(), dir_y.x()).normalized();
        Vec2d  cntr  = 0.5 * (pt1_site.cast<double>() + pt2_site.cast<double>());
        double t1 = (voronoi_point1 - cntr).dot(dir_x);
        double t2 = (voronoi_point2 - cntr).dot(dir_x);
        if (t1 > t2) {
            t1 = -t1;
            t2 = -t2;
            dir_x = - dir_x;
        }
        auto x = 0.5 * dir_y.norm() * tan_alpha_half;
        static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        auto out = std::make_pair(Vec2d(nan, nan), Vec2d(nan, nan));
        if (t2 > -x && t1 < x) {
            // Intervals overlap.
            dir_x *= x;
            out.first  = (t1 < -x) ? cntr - dir_x : voronoi_point1;
            out.second = (t2 > +x) ? cntr + dir_x : voronoi_point2;
        }
        return out;
    }

    std::pair<Vec2d, Vec2d> point_segment_skeleton_thresholds(
        // Two Voronoi sites
        const Point &pt_site, const Line &line_site,
        // End points of a Voronoi segment
        const Vec2d &voronoi_point1, const Vec2d &voronoi_point2,
        // Threshold of the skeleton function.
        const double threshold_cos_alpha)
    {
        // Foot point of the point site on the line site.
        Vec2d  ft = Geometry::foot_pt(line_site, pt_site);
        // Minimum distance of the bisector (parabolic arc) from the two sites, squared.
        Vec2d  dir_pt_ft = pt_site.cast<double>() - ft;
        // Distance of Voronoi point site from the Voronoi line site.
        double l  = dir_pt_ft.norm();
        static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
        auto   out = std::make_pair(Vec2d(nan, nan), Vec2d(nan, nan));
        // +x, -x are the two parameters along the line_site, where threshold is met.
        double r  = l / (1. + threshold_cos_alpha);
        double x2 = r * r - Slic3r::sqr(l - r);
        double x  = sqrt(x2);
        // Project voronoi_point1/2 to line_site.
        Vec2d  dir_x = (line_site.b - line_site.a).cast<double>().normalized();
        double t1 = (voronoi_point1 - ft).dot(dir_x);
        double t2 = (voronoi_point2 - ft).dot(dir_x);
        if (t1 > t2) {
            t1 = -t1;
            t2 = -t2;
            dir_x = - dir_x;
        }
        if (t2 > -x && t1 < x) {
            // Intervals overlap.
            bool t1_valid = t1 < -x;
            bool t2_valid = t2 > +x;
            // Direction of the Y axis of the parabola.
            Vec2d dir_y(- dir_x.y(), dir_x.x());
            // Orient the Y axis towards the point site.
            if (dir_y.dot(dir_pt_ft) < 0.)
                dir_y = - dir_y;
            // Equation of the parabola: y = b + a * x^2
            double a = 0.5 / l;
            dir_x *= x;
            dir_y *= 0.5 * l + a * x2;
            out.first  = t1_valid ? ft - dir_x + dir_y : voronoi_point1;
            out.second = t2_valid ? ft + dir_x + dir_y : voronoi_point2;
        }
        return out;
    }

} // namespace detail

#ifndef NDEBUG
namespace debug
{
    // Verify that twin halfedges are stored next to the other in vd.
    bool verify_twin_halfedges_successive(const VD &vd, const Lines &lines)
    {
        for (size_t i = 0; i < vd.num_edges(); i += 2) {
            const VD::edge_type &e  = vd.edges()[i];
            const VD::edge_type &e2 = vd.edges()[i + 1];
            assert(e.twin() == &e2);
            assert(e2.twin() == &e);
            assert(e.is_secondary() == e2.is_secondary());
            if (e.is_secondary()) {
                assert(e.cell()->contains_point() != e2.cell()->contains_point());
                const VD::edge_type &ex = (e.cell()->contains_point() ? e : e2);
                // Verify that the Point defining the cell left of ex is an end point of a segment
                // defining the cell right of ex.
                const Line  &line0 = lines[ex.cell()->source_index()];
                const Line  &line1 = lines[ex.twin()->cell()->source_index()];
                const Point &pt    = (ex.cell()->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                assert(pt == line1.a || pt == line1.b);
            }
        }
        return true;
    }

    bool verify_inside_outside_annotations(const VD &vd)
    {
        // Verify that "Colors" are set at all Voronoi entities.
        for (const VD::vertex_type &v : vd.vertices()) {
            assert(! v.is_degenerate());
            assert(vertex_category(v) != VertexCategory::Unknown);
        }
        for (const VD::edge_type &e : vd.edges())
            assert(edge_category(e) != EdgeCategory::Unknown);
        for (const VD::cell_type &c : vd.cells()) {
            // Unfortunately denegerate cells could be created, which reference a null edge.
            // https://github.com/boostorg/polygon/issues/47
            assert(c.is_degenerate() || cell_category(c) != CellCategory::Unknown);
        }

        // Verify consistency between markings of Voronoi cells, edges and verticies.
        for (const VD::cell_type &cell : vd.cells()) {
            if (cell.is_degenerate()) {
                // Unfortunately denegerate cells could be created, which reference a null edge.
                // https://github.com/boostorg/polygon/issues/47
                continue;
            }
            const VD::edge_type *first_edge = cell.incident_edge();
            const VD::edge_type *edge       = first_edge;
            CellCategory         cc         = cell_category(cell);
            size_t               num_vertices_on_contour    = 0;
            size_t               num_vertices_inside        = 0;
            size_t               num_vertices_outside       = 0;
            size_t               num_edges_point_to_contour = 0;
            size_t               num_edges_point_inside     = 0;
            size_t               num_edges_point_outside    = 0;
            do {
                {
                    EdgeCategory ec = edge_category(edge);
                    switch (ec) {
                    case EdgeCategory::PointsInside:
                        assert(edge->vertex0() != nullptr && edge->vertex1() != nullptr);
                        ++ num_edges_point_inside; break;
                    case EdgeCategory::PointsOutside:
//                        assert(edge->vertex0() != nullptr);
                        ++ num_edges_point_outside; break;
                    case EdgeCategory::PointsToContour:
                        assert(edge->vertex1() != nullptr);
                        ++ num_edges_point_to_contour; break;
                    default:
                        assert(false);
                    }
                }
                {
                    VertexCategory vc = (edge->vertex1() == nullptr) ? VertexCategory::Outside : vertex_category(edge->vertex1());
                    switch (vc) {
                    case VertexCategory::Inside:
                        ++ num_vertices_inside; break;
                    case VertexCategory::Outside:
                        ++ num_vertices_outside; break;
                    case VertexCategory::OnContour:
                        ++ num_vertices_on_contour; break;
                    default:
                        assert(false);
                    }
                }
                {
                    const VD::cell_type *cell_other = edge->twin()->cell();
                    const CellCategory   cc_other   = cell_category(cell_other);
                    assert(cc_other != CellCategory::Unknown);
                    switch (cc) {
                    case CellCategory::Boundary:
                        assert(cc_other != CellCategory::Boundary || cell_other->contains_segment());
                        break;
                    case CellCategory::Inside:
                        assert(cc_other == CellCategory::Inside || cc_other ==CellCategory::Boundary);
                        break;
                    case CellCategory::Outside:
                        assert(cc_other == CellCategory::Outside || cc_other == CellCategory::Boundary);
                        break;
                    default:
                        assert(false);
                        break;
                    }
                }
                edge = edge->next();
            } while (edge != first_edge);

            switch (cc) {
            case CellCategory::Boundary:
                assert(cell.contains_segment());
                assert(num_edges_point_to_contour == 2);
                assert(num_vertices_on_contour == 2);
                assert(num_vertices_inside > 0);
                assert(num_vertices_outside > 0);
                assert(num_edges_point_inside > 0);
                assert(num_edges_point_outside > 0);
                break;
            case CellCategory::Inside:
                assert(num_vertices_on_contour <= 1);
                assert(num_edges_point_to_contour <= 1);
                assert(num_vertices_inside > 0);
                assert(num_vertices_outside == 0);
                assert(num_edges_point_inside > 0);
                assert(num_edges_point_outside == 0);
                break;
            case CellCategory::Outside:
                assert(num_vertices_on_contour <= 1);
                assert(num_edges_point_to_contour <= 1);
                assert(num_vertices_inside == 0);
                assert(num_vertices_outside > 0);
                assert(num_edges_point_inside == 0);
                assert(num_edges_point_outside > 0);
                break;
            default:
                assert(false);
                break;
            }
        }

        return true;
    }

    bool verify_vertices_on_contour(const VD &vd, const Lines &lines)
    {
        for (const VD::edge_type &edge : vd.edges()) {
            const VD::vertex_type *v = edge.vertex0();
            if (v != nullptr) {
                bool on_contour = vertex_category(v) == VertexCategory::OnContour;
                assert(detail::on_site(lines, *edge.cell(), vertex_point(v)) == on_contour);
                assert(detail::on_site(lines, *edge.twin()->cell(), vertex_point(v)) == on_contour);
            }
        }
        return true;
    }

    bool verify_signed_distances(const VD &vd, const Lines &lines, const std::vector<double> &signed_distances)
    {
        for (const VD::edge_type &edge : vd.edges()) {
            const VD::vertex_type *v = edge.vertex0();
            double d = (v == nullptr) ? std::numeric_limits<double>::max() : signed_distances[v - &vd.vertices().front()];
            if (v == nullptr || vertex_category(v) == VertexCategory::Outside)
                assert(d > 0.);
            else if (vertex_category(v) == VertexCategory::OnContour)
                assert(d == 0.);
            else
                assert(d < 0.);
            if (v != nullptr) {
                double err  = std::abs(detail::dist_to_site(lines, *edge.cell(), vertex_point(v)) - std::abs(d));
                double err2 = std::abs(detail::dist_to_site(lines, *edge.twin()->cell(), vertex_point(v)) - std::abs(d));
                assert(err < SCALED_EPSILON);
                assert(err2 < SCALED_EPSILON);
            }
        }
        return true;
    }

    bool verify_offset_intersection_points(const VD &vd, const Lines &lines, const double offset_distance, const std::vector<Vec2d> &offset_intersection_points)
    {
        const VD::edge_type *front_edge = &vd.edges().front();
        const double d = std::abs(offset_distance);
        for (const VD::edge_type &edge : vd.edges()) {
            const Vec2d &p = offset_intersection_points[&edge - front_edge];
            if (edge_offset_has_intersection(p)) {
                double err  = std::abs(detail::dist_to_site(lines, *edge.cell(), p) - d);
                double err2 = std::abs(detail::dist_to_site(lines, *edge.twin()->cell(), p) - d);
                assert(err < SCALED_EPSILON);
                assert(err2 < SCALED_EPSILON);
            }
        }
        return true;
    }

}
#endif // NDEBUG

void reset_inside_outside_annotations(VD &vd)
{
    for (const VD::vertex_type &v : vd.vertices())
        set_vertex_category(const_cast<VD::vertex_type&>(v), VertexCategory::Unknown);
    for (const VD::edge_type &e : vd.edges())
        set_edge_category(const_cast<VD::edge_type&>(e), EdgeCategory::Unknown);
    for (const VD::cell_type &c : vd.cells())
        set_cell_category(const_cast<VD::cell_type&>(c), CellCategory::Unknown);
}

void annotate_inside_outside(VD &vd, const Lines &lines)
{
    assert(debug::verify_twin_halfedges_successive(vd, lines));

    reset_inside_outside_annotations(vd);

#ifdef VORONOI_DEBUG_OUT
    BoundingBox bbox;
    {
        bbox.merge(get_extents(lines));
        bbox.min -= (0.01 * bbox.size().cast<double>()).cast<coord_t>();
        bbox.max += (0.01 * bbox.size().cast<double>()).cast<coord_t>();
    }
    static int irun = 0;
    ++ irun;
    dump_voronoi_to_svg(debug_out_path("voronoi-offset-initial-%d.svg", irun).c_str(), vd, Points(), lines);
#endif // VORONOI_DEBUG_OUT

    // Set a VertexCategory, verify validity of the operation.
    auto annotate_vertex = [](const VD::vertex_type *vertex, VertexCategory new_vertex_category) {
#ifndef NDEBUG
        VertexCategory vc = vertex_category(vertex);
        assert(vc == VertexCategory::Unknown || vc == new_vertex_category);
        assert(new_vertex_category == VertexCategory::Inside || 
               new_vertex_category == VertexCategory::Outside ||
               new_vertex_category == VertexCategory::OnContour);
#endif // NDEBUG
        set_vertex_category(const_cast<VD::vertex_type*>(vertex), new_vertex_category);
    };

    // Set an EdgeCategory, verify validity of the operation.
    auto annotate_edge = [](const VD::edge_type *edge, EdgeCategory new_edge_category) {
#ifndef NDEBUG
        EdgeCategory ec = edge_category(edge);
        assert(ec == EdgeCategory::Unknown || ec == new_edge_category);
        switch (new_edge_category) {
        case EdgeCategory::PointsInside:
            assert(edge->vertex0() != nullptr);
            assert(edge->vertex1() != nullptr);
            break;
        case EdgeCategory::PointsOutside:
            // assert(edge->vertex0() != nullptr);
            break;
        case EdgeCategory::PointsToContour:
            assert(edge->vertex1() != nullptr);
            break;
        default:
            assert(false);
        }
#endif // NDEBUG
        set_edge_category(const_cast<VD::edge_type*>(edge), new_edge_category);
    };

    // Set a CellCategory, verify validity of the operation.
    // Handle marking of boundary cells (first time the cell is marked as outside, the other time as inside).
    // Returns true if the current cell category was modified.
    auto annotate_cell = [](const VD::cell_type *cell, CellCategory new_cell_category) -> bool {
        CellCategory cc = cell_category(cell);
        assert(cc == CellCategory::Inside || cc == CellCategory::Outside || cc == CellCategory::Boundary || cc == CellCategory::Unknown);
        assert(new_cell_category == CellCategory::Inside || new_cell_category == CellCategory::Outside || new_cell_category == CellCategory::Boundary);
        switch (cc) {
        case CellCategory::Unknown:
            // Old category unknown, just write the new category.
            break;
        case CellCategory::Outside:
            if (new_cell_category == CellCategory::Inside)
                new_cell_category = CellCategory::Boundary;
            break;
        case CellCategory::Inside:
            if (new_cell_category == CellCategory::Outside)
                new_cell_category = CellCategory::Boundary;
            break;
        case CellCategory::Boundary:
            return false;
        }
        if (cc != new_cell_category) {
            set_cell_category(const_cast<VD::cell_type*>(cell), new_cell_category);
            return true;
        }
        return false;
    };

    // The next loop is supposed to annotate the "on input contour" vertices, but due to
    // merging very close Voronoi vertices together by boost::polygon Voronoi generator
    // the condition may not always be met. It should be safe to mark all Voronoi very close
    // to the input contour as on contour.
    for (const VD::edge_type &edge : vd.edges()) {
        const VD::vertex_type *v = edge.vertex0();
        if (v != nullptr) {
            bool on_contour = detail::on_site(lines, *edge.cell(), vertex_point(v));
#ifndef NDEBUG
            bool on_contour2 = detail::on_site(lines, *edge.twin()->cell(), vertex_point(v));
            assert(on_contour == on_contour2);
#endif // NDEBUG
            if (on_contour)
                annotate_vertex(v, VertexCategory::OnContour);
        }
    }

    // One side of a secondary edge is always on the source contour. Mark these vertices as OnContour.
    // See the comment at the loop before, the condition may not always be met.
    for (const VD::edge_type &edge : vd.edges()) {
        if (edge.is_secondary() && edge.vertex0() != nullptr) {
            assert(edge.is_linear());
            assert(edge.cell()->contains_point() != edge.twin()->cell()->contains_point());
            // The point associated with the point site shall be equal with one vertex of this Voronoi edge.
            const Point &pt_on_contour = edge.cell()->contains_point() ? contour_point(*edge.cell(), lines) : contour_point(*edge.twin()->cell(), lines);
            auto on_contour = [&pt_on_contour](const VD::vertex_type *v) { return detail::vertex_equal_to_point(v, pt_on_contour); };
            if (edge.vertex1() == nullptr) {
                assert(on_contour(edge.vertex0()));
                annotate_vertex(edge.vertex0(), VertexCategory::OnContour);
            } else {
                // Only one of the two vertices may lie on input contour.
                const VD::vertex_type  *v0          = edge.vertex0();
                const VD::vertex_type  *v1          = edge.vertex1();
#ifndef NDEBUG
                VertexCategory          v0_category = vertex_category(v0);
                VertexCategory          v1_category = vertex_category(v1);
                assert(v0_category != VertexCategory::OnContour || v1_category != VertexCategory::OnContour);
                assert(! (on_contour(v0) && on_contour(v1)));
#endif // NDEBUG
                if (on_contour(v0))
                    annotate_vertex(v0, VertexCategory::OnContour);
                else {
                    assert(on_contour(v1));
                    annotate_vertex(v1, VertexCategory::OnContour);
                }
            }
        }
    }

    assert(debug::verify_vertices_on_contour(vd, lines));

    for (const VD::edge_type &edge : vd.edges())
        if (edge.vertex1() == nullptr) {
            // Infinite Voronoi edge separating two Point sites or a Point site and a Segment site.
            // Infinite edge is always outside and it references at least one valid vertex.
            assert(edge.is_infinite());
            assert(edge.is_linear());
            assert(edge.vertex0() != nullptr);
            const VD::cell_type *cell  = edge.cell();
            const VD::cell_type *cell2 = edge.twin()->cell();
            // A Point-Segment secondary Voronoi edge touches the input contour, a Point-Point Voronoi
            // edge does not.
            assert(edge.is_secondary() ? (cell->contains_segment() != cell2->contains_segment()) :
                                         (cell->contains_point() == cell2->contains_point()));
            annotate_edge(&edge, EdgeCategory::PointsOutside);
            // Opposite edge of an infinite edge is certainly not active.
            annotate_edge(edge.twin(), edge.is_secondary() ? EdgeCategory::PointsToContour : EdgeCategory::PointsOutside);
            annotate_vertex(edge.vertex0(), edge.is_secondary() ? VertexCategory::OnContour : VertexCategory::Outside);
            // edge.vertex1() is null, it is implicitely outside.
            if (cell->contains_segment())
                std::swap(cell, cell2);
            // State of a cell containing a boundary point is certainly outside.
            assert(cell->contains_point());
            annotate_cell(cell, CellCategory::Outside);
            assert(edge.is_secondary() == cell2->contains_segment());
            annotate_cell(cell2, cell2->contains_point() ? CellCategory::Outside : CellCategory::Boundary);
        } else if (edge.vertex0() != nullptr) {
            assert(edge.is_finite());
            const VD::cell_type *cell = edge.cell();
            const Line          *line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            if (line == nullptr) {
                cell = edge.twin()->cell();
                line = cell->contains_segment() ? &lines[cell->source_index()] : nullptr;
            }
            // Only one of the two vertices may lie on input contour.
            assert(! edge.is_linear() || vertex_category(edge.vertex0()) != VertexCategory::OnContour || vertex_category(edge.vertex1()) != VertexCategory::OnContour);
            // Now classify the Voronoi vertices and edges as inside outside, if at least one Voronoi
            // site is a Segment site.
            // Inside / outside classification of Point - Point Voronoi edges will be done later
            // by a propagation (seed fill).
            if (line) {
                const VD::vertex_type *v1    = edge.vertex1();
                const VD::cell_type   *cell2 = (cell == edge.cell()) ? edge.twin()->cell() : edge.cell();
                assert(v1 != nullptr);
                VertexCategory         v0_category = vertex_category(edge.vertex0());
                VertexCategory         v1_category = vertex_category(edge.vertex1());
                bool                   on_contour  = v0_category == VertexCategory::OnContour || v1_category == VertexCategory::OnContour;
#ifndef NDEBUG
                if (! on_contour && cell == edge.cell() && edge.twin()->cell()->contains_segment()) {
                    // Constrained bisector of two segments. Vojtech is not quite sure whether the Voronoi generator is robust enough
                    // to always connect at least one secondary edge to an input contour point. Catch it here.
                    assert(edge.is_linear());
                    // OnContour state of this edge is not known yet.
                    const Point *pt_on_contour = nullptr;
                    // If the two segments share a point, then one end of the current Voronoi edge shares this point as well.
                    // A bisector may not necessarily connect to the source contour. Find pt_on_contour if it exists.
                    const Line &line2 = lines[cell2->source_index()];
                    if (line->a == line2.b)
                        pt_on_contour = &line->a;
                    else if (line->b == line2.a)
                        pt_on_contour = &line->b;
                    if (pt_on_contour) {
                        const VD::vertex_type *v0 = edge.vertex0();
                        auto on_contour = [&pt_on_contour](const VD::vertex_type *v) {
                            return std::abs(v->x() - pt_on_contour->x()) < 0.5001 &&
                                   std::abs(v->y() - pt_on_contour->y()) < 0.5001;
                        };
                        assert(! on_contour(v0) && ! on_contour(v1));
                    }
                }
#endif // NDEBUG
                if (on_contour && v1_category == VertexCategory::OnContour) {
                    // Skip secondary edge pointing to a contour point.
                    annotate_edge(&edge, EdgeCategory::PointsToContour);
                } else {
                    // v0 is certainly not on the input polygons.
                    // Is v1 inside or outside the input polygons?
                    // The Voronoi vertex coordinate is in doubles, calculate orientation in doubles.
                    Vec2d l0(line->a.cast<double>());
                    Vec2d lv((line->b - line->a).cast<double>());
                    double side = cross2(Vec2d(v1->x(), v1->y()) - l0, lv);
                    // No Voronoi edge could connect two vertices of input polygons.
                    assert(side != 0.);
                    auto vc = side > 0. ? VertexCategory::Outside : VertexCategory::Inside;
                    annotate_vertex(v1, vc);
                    auto ec = vc == VertexCategory::Outside ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside;
                    annotate_edge(&edge, ec);
                    // Annotate the twin edge and its vertex. As the Voronoi edge may never cross the input
                    // contour, the twin edge and its vertex will share the property of edge.
                    annotate_vertex(edge.vertex0(), on_contour ? VertexCategory::OnContour : vc);
                    annotate_edge(edge.twin(), on_contour ? EdgeCategory::PointsToContour : ec);
                    assert(cell->contains_segment());
                    annotate_cell(cell, on_contour ? CellCategory::Boundary :
                        (vc == VertexCategory::Outside ? CellCategory::Outside : CellCategory::Inside));
                    annotate_cell(cell2, (on_contour && cell2->contains_segment()) ? CellCategory::Boundary :
                        (vc == VertexCategory::Outside ? CellCategory::Outside : CellCategory::Inside));
                }
            }
        }

    assert(debug::verify_vertices_on_contour(vd, lines));

    // Now most Voronoi vertices, edges and cells are annotated, with the exception of some
    // edges separating two Point sites, their cells and vertices.
    // Perform one round of expansion marking Voronoi edges and cells next to boundary cells.
    std::vector<const VD::cell_type*> cell_queue;
    for (const VD::edge_type &edge : vd.edges()) {
        assert((edge_category(edge) == EdgeCategory::Unknown) == (edge_category(edge.twin()) == EdgeCategory::Unknown));
        if (edge_category(edge) == EdgeCategory::Unknown) {
            assert(edge.is_finite());
            const VD::cell_type &cell   = *edge.cell();
            const VD::cell_type &cell2  = *edge.twin()->cell();
            assert(cell.contains_point() && cell2.contains_point());
            CellCategory         cc     = cell_category(cell);
            CellCategory         cc2    = cell_category(cell2);
            assert(cc != CellCategory::Boundary && cc2 != CellCategory::Boundary);
            CellCategory         cc_new = cc;
            if (cc_new == CellCategory::Unknown)
                cc_new = cc2;
            else
                assert(cc2 == CellCategory::Unknown || cc == cc2);
            if (cc_new == CellCategory::Unknown) {
                VertexCategory vc = vertex_category(edge.vertex0());
                assert(vc != VertexCategory::OnContour);
                if (vc != VertexCategory::Unknown)
                    cc_new = (vc == VertexCategory::Outside) ? CellCategory::Outside : CellCategory::Inside;
            }
            if (cc_new != CellCategory::Unknown) {
                VertexCategory vc = (cc_new == CellCategory::Outside) ? VertexCategory::Outside : VertexCategory::Inside;
                annotate_vertex(edge.vertex0(), vc);
                annotate_vertex(edge.vertex1(), vc);
                auto ec_new = (cc_new == CellCategory::Outside) ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside;
                annotate_edge(&edge, ec_new);
                annotate_edge(edge.twin(), ec_new);
                if (cc != cc_new) {
                    annotate_cell(&cell, cc_new);
                    cell_queue.emplace_back(&cell);
                }
                if (cc2 != cc_new) {
                    annotate_cell(&cell2, cc_new);
                    cell_queue.emplace_back(&cell2);
                }
            }
        }
    }

    assert(debug::verify_vertices_on_contour(vd, lines));

    // Do a final seed fill over Voronoi cells and unmarked Voronoi edges.
    while (! cell_queue.empty()) {
        const VD::cell_type *cell = cell_queue.back();
        const CellCategory   cc   = cell_category(cell);
        assert(cc == CellCategory::Outside || cc == CellCategory::Inside);
        cell_queue.pop_back();
        const VD::edge_type *first_edge = cell->incident_edge();
        const VD::edge_type *edge       = first_edge;
        const auto           ec_new     = (cc == CellCategory::Outside) ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside;
        do {
            EdgeCategory ec = edge_category(edge);
            if (ec == EdgeCategory::Unknown) {
                assert(edge->cell()->contains_point() && edge->twin()->cell()->contains_point());
                annotate_edge(edge, ec_new);
                annotate_edge(edge->twin(), ec_new);
                const VD::cell_type *cell2 = edge->twin()->cell();
                CellCategory cc2 = cell_category(cell2);
                assert(cc2 == CellCategory::Unknown || cc2 == cc);
                if (cc2 != cc) {
                    annotate_cell(cell2, cc);
                    cell_queue.emplace_back(cell2);
                }
            } else {
                assert(edge->vertex0() == nullptr || vertex_category(edge->vertex0()) != VertexCategory::Unknown);
                assert(edge->vertex1() == nullptr || vertex_category(edge->vertex1()) != VertexCategory::Unknown);
                assert(edge_category(edge->twin()) != EdgeCategory::Unknown);
                assert(cell_category(edge->cell()) != CellCategory::Unknown);
                assert(cell_category(edge->twin()->cell()) != CellCategory::Unknown);
            }
            edge = edge->next();
        } while (edge != first_edge);
    }

    assert(debug::verify_vertices_on_contour(vd, lines));
    assert(debug::verify_inside_outside_annotations(vd));
}

std::vector<double> signed_vertex_distances(const VD &vd, const Lines &lines)
{
    // vd shall be annotated.
    assert(debug::verify_inside_outside_annotations(vd));

    std::vector<double> out(vd.vertices().size(), 0.);
    const VD::vertex_type *first_vertex = &vd.vertices().front();
    for (const VD::vertex_type &vertex : vd.vertices()) {
        const VertexCategory vc = vertex_category(vertex);
        double dist;
        if (vc == VertexCategory::OnContour) {
            dist = 0.;
        } else {
            const VD::edge_type *first_edge = vertex.incident_edge();
            const VD::edge_type *edge       = first_edge;
            const VD::cell_type *point_cell = nullptr;
            do {
                if (edge->cell()->contains_point()) {
                    point_cell = edge->cell();
                    break;
                }
                edge = edge->rot_next();
            } while (edge != first_edge);
            if (point_cell == 0) {
                // Project vertex onto a contour segment.
                const Line &line = lines[edge->cell()->source_index()];
                dist = Geometry::ray_point_distance<Vec2d>(
                    line.a.cast<double>(), (line.b - line.a).cast<double>(), vertex_point(vertex));
            } else {
                // Distance to a contour point.
                dist = (contour_point(*point_cell, lines).cast<double>() - vertex_point(vertex)).norm();
            }
            if (vc == VertexCategory::Inside)
                dist = - dist;
        }
        out[&vertex - first_vertex] = dist;
    }

    assert(debug::verify_signed_distances(vd, lines, out));

    return out;
}

std::vector<Vec2d> edge_offset_contour_intersections(
    const VD                    &vd,
    const Lines                 &lines,
    const std::vector<double>   &vertex_distances,
    double                       offset_distance)
{
    // vd shall be annotated.
    assert(debug::verify_inside_outside_annotations(vd));

    bool outside = offset_distance > 0;
    if (! outside)
        offset_distance = - offset_distance;
    assert(offset_distance > 0.);

    const VD::vertex_type *first_vertex = &vd.vertices().front();
    const VD::edge_type   *first_edge   = &vd.edges().front();
    static constexpr double nan         = std::numeric_limits<double>::quiet_NaN();
    // By default none edge has an intersection with the offset curve.
    std::vector<Vec2d>     out(vd.num_edges(), Vec2d(nan, 0.));

    for (const VD::edge_type &edge : vd.edges()) {
        size_t edge_idx = &edge - first_edge;
        if (edge_offset_has_intersection(out[edge_idx]) || out[edge_idx].y() != 0.)
            // This edge was already classified.
            continue;

        const VD::vertex_type *v0 = edge.vertex0();
        const VD::vertex_type *v1 = edge.vertex1();
        if (v0 == nullptr) {
            assert(vertex_category(v1) == VertexCategory::OnContour || vertex_category(v1) == VertexCategory::Outside);
            continue;
        }

        double d0 = (v0 == nullptr) ? std::numeric_limits<double>::max() : vertex_distances[v0 - first_vertex];
        double d1 = (v1 == nullptr) ? std::numeric_limits<double>::max() : vertex_distances[v1 - first_vertex];
        assert(d0 * d1 >= 0.);
        if (! outside) {
            d0 = - d0;
            d1 = - d1;
        }
#ifndef NDEBUG
        {
            double err  = std::abs(detail::dist_to_site(lines, *edge.cell(), vertex_point(v0)) - std::abs(d0));
            double err2 = std::abs(detail::dist_to_site(lines, *edge.twin()->cell(), vertex_point(v0)) - std::abs(d0));
            assert(err < SCALED_EPSILON);
            assert(err2 < SCALED_EPSILON);
            if (v1 != nullptr) {
                double err3 = std::abs(detail::dist_to_site(lines, *edge.cell(), vertex_point(v1)) - std::abs(d1));
                double err4 = std::abs(detail::dist_to_site(lines, *edge.twin()->cell(), vertex_point(v1)) - std::abs(d1));
                assert(err3 < SCALED_EPSILON);
                assert(err4 < SCALED_EPSILON);
            }
        }
#endif // NDEBUG
        double dmin, dmax;
        if (d0 < d1)
            dmin = d0, dmax = d1;
        else
            dmax = d0, dmin = d1;
        // Offset distance may be lower than dmin, but never higher than dmax.
        // Don't intersect an edge at dmax
        //      1) To avoid zero edge length, zero area offset contours.
        //      2) To ensure that the offset contours that cross a Voronoi vertex are traced consistently
        //         at one side of the offset curve only.
        if (offset_distance >= dmax)
            continue;

        // Edge candidate, intersection points were not calculated yet.
        assert(v0 != nullptr);
        const VD::cell_type   *cell  = edge.cell();
        const VD::cell_type   *cell2 = edge.twin()->cell();
        const Line            &line0 = lines[cell->source_index()];
        const Line            &line1 = lines[cell2->source_index()];
        size_t                 edge_idx2 = edge.twin() - first_edge;
        if (v1 == nullptr) {
            assert(edge.is_infinite());
            assert(edge.is_linear());
            // Unconstrained edges have always montonous distance.
            assert(d0 != d1);
            if (offset_distance > dmin) {
                // There is certainly an intersection with the offset curve.
                if (cell->contains_point() && cell2->contains_point()) {
                    assert(! edge.is_secondary());
                    const Point &pt0 = contour_point(*cell, line0);
                    const Point &pt1 = contour_point(*cell2, line1);
                    // pt is inside the circle (pt0, offset_distance), (pt + dir) is certainly outside the same circle.
                    Vec2d dir = Vec2d(double(pt0.y() - pt1.y()), double(pt1.x() - pt0.x())) * (2. * offset_distance);
                    Vec2d pt(v0->x(), v0->y());
                    double t = detail::first_circle_segment_intersection_parameter(Vec2d(pt0.x(), pt0.y()), offset_distance, pt, dir);
                    assert(t > 0.);
                    out[edge_idx] = pt + t * dir;
                } else {
                    // Infinite edges could not be created by two segment sites.
                    assert(cell->contains_point() != cell2->contains_point());
                    // Linear edge goes through the endpoint of a segment.
                    assert(edge.is_secondary());
                    const Point &ipt = cell->contains_segment() ? contour_point(*cell2, line1) : contour_point(*cell, line0);
    #ifndef NDEBUG
                    if (cell->contains_segment()) {
                        const Point &pt1 = contour_point(*cell2, line1);
                        assert(pt1 == line0.a || pt1 == line0.b);
                    } else {
                        const Point &pt0 = contour_point(*cell, line0);
                        assert(pt0 == line1.a || pt0 == line1.b);
                    }
                    assert((vertex_point(v0) - ipt.cast<double>()).norm() < SCALED_EPSILON);
    #endif /* NDEBUG */
                    // Infinite edge starts at an input contour, therefore there is always an intersection with an offset curve.
                    const Line &line = cell->contains_segment() ? line0 : line1;
                    assert(line.a == ipt || line.b == ipt);
                    out[edge_idx] = ipt.cast<double>() + offset_distance * Vec2d(line.b.y() - line.a.y(), line.a.x() - line.b.x()).normalized();
                }
            } else if (offset_distance == dmin)
                out[edge_idx] = vertex_point(v0);
            // The other edge of an unconstrained edge starting with null vertex shall never be intersected. Mark it as visited.
            out[edge_idx2].y() = 1.;
        } else {
            assert(edge.is_finite());
            bool done = false;
            // Bisector of two line segments, distance along the bisector is linear.
            bool bisector = cell->contains_segment() && cell2->contains_segment();
            assert(edge.is_finite());
            // or a secondary line, again the distance along the secondary line is linear and starts at the contour (zero distance).
            if (bisector || edge.is_secondary()) {
                assert(edge.is_linear());
#ifndef NDEBUG
                if (edge.is_secondary()) {
                    assert(cell->contains_point() != cell2->contains_point());
                    // One of the vertices is on the input contour.
                    assert((vertex_category(edge.vertex0()) == VertexCategory::OnContour) !=
                           (vertex_category(edge.vertex1()) == VertexCategory::OnContour));
                    assert(dmin == 0.);
                }
#endif // NDEBUG
                if (! bisector || (dmin != dmax && offset_distance >= dmin)) {
                    double t = (offset_distance - dmin) / (dmax - dmin);
                    t = std::clamp(t, 0., 1.);
                    if (d1 < d0) {
                        out[edge_idx2] = Slic3r::lerp(vertex_point(v1), vertex_point(v0), t);
                        // mark visited
                        out[edge_idx].y() = 1.;
                    } else {
                        out[edge_idx] = Slic3r::lerp(vertex_point(v0), vertex_point(v1), t);
                        // mark visited
                        out[edge_idx2].y() = 1.;
                    }
                    done = true;
                }
            } else {
                // Point - Segment or Point - Point edge, distance along this Voronoi edge may not be monotonous:
                // The distance function along the Voronoi edge may either have one maximum at one vertex and one minimum at the other vertex,
                // or it may have two maxima at the vertices and a minimum somewhere along the Voronoi edge, and this Voronoi edge
                // may be intersected twice by an offset curve.
                //
                // Tracing an offset curve accross Voronoi regions with linear edges of montonously increasing or decrasing distance
                // to a Voronoi region is stable in a sense, that if the distance of Voronoi vertices is calculated correctly, there will
                // be maximum one intersection of an offset curve found at each Voronoi edge and tracing these intersections shall
                // produce a set of closed curves.
                //
                // Having a non-monotonous distance function between the Voronoi edge end points may lead to splitting of offset curves
                // at these Voronoi edges. If a Voronoi edge is classified as having no intersection at all while it has some,
                // the extracted offset curve will contain self intersections at this Voronoi edge.
                //
                // If on the other side the two intersection points are found by a numerical error even though none should be found, then
                // it may happen that it would not be possible to connect these two points into a closed loop, which is likely worse
                // than the issue above.
                //
                // While it is likely not possible to avoid all the numerical issues, one shall strive for the highest numerical robustness.
                assert(cell->contains_point() || cell2->contains_point());
                size_t       num_intersections  = 0;
                bool         point_vs_segment   = cell->contains_point() != cell2->contains_point();
                const Point &pt0                = cell->contains_point() ? contour_point(*cell, line0) : contour_point(*cell2, line1);
                // Project p0 to line segment <v0, v1>.
                Vec2d p0(v0->x(), v0->y());
                Vec2d p1(v1->x(), v1->y());
                Vec2d px(pt0.x(), pt0.y());
                const Point *pt1 = nullptr;
                Vec2d dir;
                if (point_vs_segment) {
                    const Line &line = cell->contains_segment() ? line0 : line1;
                    dir = (line.b - line.a).cast<double>();
                } else {
                    pt1 = &contour_point(*cell2, line1);
                    // Perpendicular to the (pt1 - pt0) direction.
                    dir = Vec2d(double(pt0.y() - pt1->y()), double(pt1->x() - pt0.x()));
                }
                double s0 = (p0 - px).dot(dir);
                double s1 = (p1 - px).dot(dir);
                if (offset_distance >= dmin) {
                    // This Voronoi edge is intersected by the offset curve just once.
                    // There may be numerical issues if dmin is close to the minimum of the non-monotonous distance function.
                    num_intersections = 1;
                } else {
                    // This Voronoi edge may not be intersected by the offset curve, or it may split the offset curve
                    // into two loops. First classify this edge robustly with regard to the Point-Segment bisector or Point-Point bisector.
                    double dmin_new;
                    bool   found = false;
                    if (point_vs_segment) {
                        if (s0 * s1 <= 0.) {
                            // One end of the Voronoi edge is on one side of the Point-Segment bisector, the other end of the Voronoi
                            // edge is on the other side of the bisector, therefore with a high probability we should find a minimum
                            // of the distance to a nearest site somewhere inside this Voronoi edge (at the intersection of the bisector
                            // and the Voronoi edge.
                            const Line &line = cell->contains_segment() ? line0 : line1;
                            dmin_new = 0.5 * (Geometry::foot_pt<Vec2d>(line.a.cast<double>(), dir, px) - px).norm();
                            found    = true;
                        }
                    } else {
                        // Point-Point Voronoi sites.
                        if (s0 * s1 <= 0.) {
                            // This Voronoi edge intersects connection line of the two Point sites.
                            dmin_new = 0.5 * (pt1->cast<double>() - px).norm();
                            found    = true;
                        }
                    }
                    if (found) {
                        assert(dmin_new < dmax + SCALED_EPSILON);
                        assert(dmin_new < dmin + SCALED_EPSILON);
                        if (dmin_new <= offset_distance) {
                            // 1) offset_distance > dmin_new -> two new distinct intersection points are found.
                            // 2) offset_distance == dmin_new -> one duplicate point is found.
                            // If 2) is ignored, then two tangentially touching offset curves are created.
                            // If not ignored, then the two offset curves merge at this double point.
                            // We should merge the contours while pushing the the two copies of the tangent point away a bit.
                            dmin = dmin_new;
                            num_intersections = (offset_distance > dmin) + 1;
                        }
                    }
                }
                if (num_intersections > 0) {
                    detail::Intersections intersections;
                    if (point_vs_segment) {
                        assert(cell->contains_point() || cell2->contains_point());
                        intersections = detail::line_point_equal_distance_points(cell->contains_segment() ? line0 : line1, pt0, offset_distance);
                    } else {
                        intersections = detail::point_point_equal_distance_points(pt0, *pt1, offset_distance);
                    }
                    // The functions above perform complex calculations in doubles, therefore the results may not be quite accurate and
                    // the number of intersections found may not be in accord to the number of intersections expected from evaluating simpler expressions.
                    // Adjust the result to the number of intersection points expected.
                    if (num_intersections == 2) {
                        switch (intersections.count) {
                        case 0:
                            // No intersection found even though one or two were expected to be found.
                            // Not trying to find the intersection means that we may produce offset curves, that intersect at this Voronoi edge.
                            //FIXME We are fine with that for now, but we may try to create artificial split points in further revisions.
                            break;
                        case 1:
                            // Tangential point found.
                            //FIXME We are fine with that for now, but we may try to create artificial split points in further revisions.
                            break;
                        default:
                        {
                            // Two intersection points found. Sort them.
                            assert(intersections.count == 2);
                            double q0 = (intersections.pts[0] - px).dot(dir);
                            double q1 = (intersections.pts[1] - px).dot(dir);
                            // Both Voronoi edge end points and offset contour intersection points should be separated by the bisector.
                            assert(q0 * q1 <= 0.);
                            assert(s0 * s1 <= 0.);
                            // Sort the intersection points by dir.
                            if ((q0 < q1) != (s0 < s1))
                                std::swap(intersections.pts[0], intersections.pts[1]);
                        }
                        }
                    } else {
                        assert(num_intersections == 1);
                        switch (intersections.count) {
                        case 0:
                            // No intersection found. This should not happen.
                            // Create one artificial intersection point by repeating the dmin point, which is supposed to be
                            // close to the minimum.
                            intersections.pts[0] = (dmin == d0) ? p0 : p1;
                            intersections.count = 1;
                            break;
                        case 1:
                            // One intersection found. This is a tangential point. Use it.
                            break;
                        default:
                            // Two intersections found.
                            // Now decide which of the point fall on this Voronoi edge.
                            assert(intersections.count == 2);
                            double q0 = (intersections.pts[0] - px).dot(dir);
                            double q1 = (intersections.pts[1] - px).dot(dir);
                            // Offset contour intersection points should be separated by the bisector.
                            assert(q0 * q1 <= 0);
                            double s = (dmax == d0) ? s0 : s1;
                            bool take_2nd = (s > 0.) ? q1 > q0 : q1 < q0;
                            if (take_2nd)
                                intersections.pts[0] = intersections.pts[1];
                            -- intersections.count;
                        }
                    }
                    assert(intersections.count > 0);
                    if (intersections.count == 2) {
                        out[edge_idx] = intersections.pts[1];
                        out[edge_idx2] = intersections.pts[0];
                        done = true;
                    } else if (intersections.count == 1) {
                        if (d1 < d0)
                            std::swap(edge_idx, edge_idx2);
                        out[edge_idx] = intersections.pts[0];
                        out[edge_idx2].y() = 1.;
                        done = true;
                    }
                }
            }
            if (! done)
                out[edge_idx].y() = out[edge_idx2].y() = 1.;
        }
    }

    assert(debug::verify_offset_intersection_points(vd, lines, offset_distance, out));

    return out;
}

Polygons offset(
    const Geometry::VoronoiDiagram  &vd,
    const Lines                     &lines,
    const std::vector<double>       &signed_vertex_distances,
    double                           offset_distance,
    double                           discretization_error)
{
#ifdef VORONOI_DEBUG_OUT
    BoundingBox bbox;
    {
        bbox.merge(get_extents(lines));
        bbox.min -= (0.01 * bbox.size().cast<double>()).cast<coord_t>();
        bbox.max += (0.01 * bbox.size().cast<double>()).cast<coord_t>();
    }
    static int irun = 0;
    ++ irun;
    {
        Lines helper_lines;
        for (const VD::edge_type &edge : vd.edges())
            if (edge_category(edge) == (offset_distance > 0 ? EdgeCategory::PointsOutside : EdgeCategory::PointsInside) &&
                edge.vertex0() != nullptr) {
                const VD::vertex_type *v0 = edge.vertex0();
                const VD::vertex_type *v1 = edge.vertex1();
                Vec2d pt1(v0->x(), v0->y());
                Vec2d pt2;
                if (v1 == nullptr) {
                    // Unconstrained edge. Calculate a trimmed position.
                    assert(edge.is_linear());
                    const VD::cell_type *cell  = edge.cell();
                    const VD::cell_type *cell2 = edge.twin()->cell();
                    const Line          &line0 = lines[cell->source_index()];
                    const Line          &line1 = lines[cell2->source_index()];
                    if (cell->contains_point() && cell2->contains_point()) {
                        const Point &pt0 = (cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b;
                        const Point &pt1 = (cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b;
                        // Direction vector of this unconstrained Voronoi edge.
                        Vec2d dir(double(pt0.y() - pt1.y()), double(pt1.x() - pt0.x()));
                        pt2 = Vec2d(v0->x(), v0->y()) + dir.normalized() * scale_(10.);
                    } else {
                        // Infinite edges could not be created by two segment sites.
                        assert(cell->contains_point() != cell2->contains_point());
                        // Linear edge goes through the endpoint of a segment.
                        assert(edge.is_secondary());
                        const Point &ipt = cell->contains_segment() ?
                            ((cell2->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line1.a : line1.b) :
                            ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b);
                        // Infinite edge starts at an input contour, therefore there is always an intersection with an offset curve.
                        const Line  &line = cell->contains_segment() ? line0 : line1;
                        assert(line.a == ipt || line.b == ipt);
                        // dir is perpendicular to line.
                        Vec2d dir(line.a.y() - line.b.y(), line.b.x() - line.a.x());
                        assert(dir.norm() > 0.);
                        if (((line.a == ipt) == cell->contains_point()) == (v0 == nullptr))
                            dir = - dir;
                        pt2 = ipt.cast<double>() + dir.normalized() * scale_(10.);
                    }
                } else {
                    pt2 = Vec2d(v1->x(), v1->y());
                    // Clip the line by the bounding box, so that the coloring of the line will be visible.
                    Geometry::liang_barsky_line_clipping(pt1, pt2, BoundingBoxf(bbox.min.cast<double>(), bbox.max.cast<double>()));
                }
                helper_lines.emplace_back(Line(Point(pt1.cast<coord_t>()), Point(((pt1 + pt2) * 0.5).cast<coord_t>())));
            }
        dump_voronoi_to_svg(debug_out_path("voronoi-offset-candidates1-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), helper_lines);
    }
#endif // VORONOI_DEBUG_OUT

    std::vector<Vec2d>   edge_points = edge_offset_contour_intersections(vd, lines, signed_vertex_distances, offset_distance);
    const VD::edge_type *front_edge  = &vd.edges().front();

#ifdef VORONOI_DEBUG_OUT
    Lines helper_lines;
    {
        for (const VD::edge_type &edge : vd.edges())
            if (edge_offset_has_intersection(edge_points[&edge - front_edge]))
                helper_lines.emplace_back(Line(Point(edge.vertex0()->x(), edge.vertex0()->y()), Point(edge_points[&edge - front_edge].cast<coord_t>())));
        dump_voronoi_to_svg(debug_out_path("voronoi-offset-candidates2-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), helper_lines);
    }
#endif // VORONOI_DEBUG_OUT

    auto next_offset_edge = [&edge_points, front_edge](const VD::edge_type *start_edge) -> const VD::edge_type* {
	    for (const VD::edge_type *edge = start_edge->next(); edge != start_edge; edge = edge->next())
            if (edge_offset_has_intersection(edge_points[edge->twin() - front_edge]))
                return edge->twin();
        // assert(false);
        return nullptr;
	};

    const bool inside_offset = offset_distance < 0.;
    if (inside_offset)
        offset_distance = - offset_distance;

    // Track the offset curves.
    Polygons out;
	double angle_step    = 2. * acos((offset_distance - discretization_error) / offset_distance);
    double cos_threshold = cos(angle_step);
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();
	for (size_t seed_edge_idx = 0; seed_edge_idx < vd.num_edges(); ++ seed_edge_idx) {
        Vec2d last_pt = edge_points[seed_edge_idx];
        if (edge_offset_has_intersection(last_pt)) {
            const VD::edge_type *start_edge = &vd.edges()[seed_edge_idx];
            const VD::edge_type *edge       = start_edge;
            Polygon  			 poly;
		    do {
		        // find the next edge
                const VD::edge_type *next_edge = next_offset_edge(edge);
#ifdef VORONOI_DEBUG_OUT
                if (next_edge == nullptr) {
                    Lines hl = helper_lines;
                    append(hl, to_lines(Polyline(poly.points)));
                    dump_voronoi_to_svg(debug_out_path("voronoi-offset-open-loop-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), hl);
                }
#endif // VORONOI_DEBUG_OUT
                assert(next_edge);
		        //std::cout << "offset-output: "; print_edge(edge); std::cout << " to "; print_edge(next_edge); std::cout << "\n";
		        // Interpolate a circular segment or insert a linear segment between edge and next_edge.
                const VD::cell_type  *cell      = edge->cell();
                // Mark the edge / offset curve intersection point as consumed.
                Vec2d p1 = last_pt;
                Vec2d p2 = edge_points[next_edge - front_edge];
                edge_points[next_edge - front_edge].x() = nan;
#ifndef NDEBUG
                {
                    double err  = detail::dist_to_site(lines, *cell, p1) - offset_distance;
                    double err2 = detail::dist_to_site(lines, *cell, p2) - offset_distance;
#ifdef VORONOI_DEBUG_OUT
                    if (std::max(err, err2) >= SCALED_EPSILON) {
                        Lines helper_lines;
                        dump_voronoi_to_svg(debug_out_path("voronoi-offset-incorrect_pt-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), to_lines(poly));
                    }
#endif // VORONOI_DEBUG_OUT
                    assert(std::abs(err) < SCALED_EPSILON);
                    assert(std::abs(err2) < SCALED_EPSILON);
                }
#endif /* NDEBUG */
				if (cell->contains_point()) {
					// Discretize an arc from p1 to p2 with radius = offset_distance and discretization_error.
                    // The extracted contour is CCW oriented, extracted holes are CW oriented.
                    // The extracted arc will have the same orientation. As the Voronoi regions are convex, the angle covered by the arc will be convex as well.
                    const Line  &line0  = lines[cell->source_index()];
					const Vec2d &center = ((cell->source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line0.a : line0.b).cast<double>();
					const Vec2d  v1 	= p1 - center;
					const Vec2d  v2 	= p2 - center;
                    bool 		 ccw    = cross2(v1, v2) > 0;
                    double       cos_a  = v1.dot(v2);
                    double       norm   = v1.norm() * v2.norm();
                    assert(norm > 0.);
                    if (cos_a < cos_threshold * norm) {
						// Angle is bigger than the threshold, therefore the arc will be discretized.
                        cos_a /= norm;
                        assert(cos_a > -1. - EPSILON && cos_a < 1. + EPSILON);
                        double angle = acos(std::max(-1., std::min(1., cos_a)));
						size_t n_steps = size_t(ceil(angle / angle_step));
						double astep = angle / n_steps;
						if (! ccw)
							astep *= -1.;
						double a = astep;
						for (size_t i = 1; i < n_steps; ++ i, a += astep) {
							double c = cos(a);
							double s = sin(a);
							Vec2d  p = center + Vec2d(c * v1.x() - s * v1.y(), s * v1.x() + c * v1.y());
                            poly.points.emplace_back(Point(coord_t(p.x()), coord_t(p.y())));
						}
                    }
				}
                {
                    Point pt_last(coord_t(p2.x()), coord_t(p2.y()));
                    if (poly.empty() || poly.points.back() != pt_last)
                        poly.points.emplace_back(pt_last);
                }
                edge = next_edge;
                last_pt = p2;
		    } while (edge != start_edge);

            while (! poly.empty() && poly.points.front() == poly.points.back())
                poly.points.pop_back();
            if (poly.size() >= 3)
                out.emplace_back(std::move(poly));
		}
    }

	return out;
}

Polygons offset(
	const VD 		&vd, 
	const Lines 	&lines, 
	double 			 offset_distance, 
	double 			 discretization_error)
{
    annotate_inside_outside(const_cast<VD&>(vd), lines);
    std::vector<double> dist = signed_vertex_distances(vd, lines);
    return offset(vd, lines, dist, offset_distance, discretization_error);
}

// Produce a list of start positions of a skeleton segment at a halfedge.
// If the whole Voronoi edge is part of the skeleton, then zero start positions are assigned
// to both ends of the edge. Position "1" shall never be assigned to a halfedge.
//
// Skeleton edges must be inside a closed polygon, therefore these edges are finite.
// A Voronoi Edge-Edge bisector is either completely part of a skeleton, or not at all.
// An infinite Voronoi Edge-Point (parabola) or Point-Point (line) bisector is split into
// a center part close to the Voronoi sites (not skeleton) and the ends (skeleton),
// though either part could be clipped by the Voronoi segment.
// 
// Further filtering of the skeleton may be necessary.
std::vector<Vec2d> skeleton_edges_rough(
    const VD                    &vd,
    const Lines                 &lines,
    // Angle threshold at a sharp convex corner, which is marked for a gap fill.
    const double                 threshold_alpha)
{
    // vd shall be annotated.
    assert(debug::verify_inside_outside_annotations(vd));

    const VD::edge_type    *first_edge   = &vd.edges().front();
    static constexpr double nan         = std::numeric_limits<double>::quiet_NaN();
    // By default no edge is annotated as being part of the skeleton.
    std::vector<Vec2d>      out(vd.num_edges(), Vec2d(nan, nan));
    // Threshold at a sharp corner, derived from a dot product of the sharp corner edges.
    const double            threshold_cos_alpha = cos(threshold_alpha);
    // For sharp corners, dr/dl = sin(alpha/2). Substituting the dr/dl threshold with tan(alpha/2) threshold
    // in Voronoi point - point and Voronoi point - line site functions.
    const double            threshold_tan_alpha_half = tan(0.5 * threshold_alpha);

    for (const VD::edge_type &edge : vd.edges()) {
        size_t edge_idx = &edge - first_edge;
        if (
            // Ignore secondary and unbounded edges, they shall never be part of the skeleton.
            edge.is_secondary() || edge.is_infinite() ||
            // Skip the twin edge of an edge, that has already been processed.
            &edge > edge.twin() ||
            // Ignore outer edges.
            (edge_category(edge) != EdgeCategory::PointsInside && edge_category(edge.twin()) != EdgeCategory::PointsInside))
            continue;
        const VD::vertex_type *v0 = edge.vertex0();
        const VD::vertex_type *v1 = edge.vertex1();
        const VD::cell_type   *cell  = edge.cell();
        const VD::cell_type   *cell2 = edge.twin()->cell();
        const Line            &line0 = lines[cell->source_index()];
        const Line            &line1 = lines[cell2->source_index()];
        size_t                 edge_idx2 = edge.twin() - first_edge;
        if (cell->contains_segment() && cell2->contains_segment()) {
            // Bisector of two line segments, distance along the bisector is linear,
            // dr/dl is constant.
            // using trigonometric identity sin^2(a) = (1-cos(2a))/2
            Vec2d  lv0 = (line0.b - line0.a).cast<double>();
            Vec2d  lv1 = (line1.b - line1.a).cast<double>();
            double d   = lv0.dot(lv1);
            if (d < 0.) {
                double cos_alpha = - d / (lv0.norm() * lv1.norm());
                if (cos_alpha > threshold_cos_alpha) {
                    // The whole bisector is a skeleton segment.
                    out[edge_idx]  = vertex_point(v0);
                    out[edge_idx2] = vertex_point(v1);
                }
            }
        } else {
            // An infinite Voronoi Edge-Point (parabola) or Point-Point (line) bisector, clipped to a finite Voronoi segment.
            // The infinite bisector has a distance (skeleton radius) minimum, which is also a minimum 
            // of the skeleton function dr / dt.
            assert(cell->contains_point() || cell2->contains_point());
            if (cell->contains_point() != cell2->contains_point()) {
                // Point - Segment
                const Point &pt0  = cell->contains_point() ? contour_point(*cell, line0) : contour_point(*cell2, line1);
                const Line  &line = cell->contains_segment() ? line0 : line1;
                std::tie(out[edge_idx], out[edge_idx2]) = detail::point_segment_dr_dl_thresholds(
                    pt0, line, vertex_point(v0), vertex_point(v1), threshold_tan_alpha_half);
            } else {
                // Point - Point
                const Point &pt0 = contour_point(*cell,  line0);
                const Point &pt1 = contour_point(*cell2, line1);
                std::tie(out[edge_idx], out[edge_idx2]) = detail::point_point_dr_dl_thresholds(
                    pt0, pt1, vertex_point(v0), vertex_point(v1), threshold_tan_alpha_half);
            }
        }
    }

#ifdef VORONOI_DEBUG_OUT
    {
        static int irun = 0;
        ++ irun;
        Lines helper_lines;
        for (const VD::edge_type &edge : vd.edges())
            if (&edge < edge.twin() && edge.is_finite()) {
                const Vec2d &skeleton_pt      = out[&edge - first_edge];
                const Vec2d &skeleton_pt2     = out[edge.twin() - first_edge];
                bool         has_skeleton_pt  = ! std::isnan(skeleton_pt.x());
                bool         has_skeleton_pt2 = ! std::isnan(skeleton_pt2.x());
                const Vec2d &vertex_pt        = vertex_point(edge.vertex0());
                const Vec2d &vertex_pt2       = vertex_point(edge.vertex1());
                if (has_skeleton_pt && has_skeleton_pt2) {
                    // Complete edge is part of the skeleton.
                    helper_lines.emplace_back(Line(Point(vertex_pt.x(), vertex_pt.y()), Point(vertex_pt2.x(), vertex_pt2.y())));
                } else {
                    if (has_skeleton_pt)
                        helper_lines.emplace_back(Line(Point(vertex_pt2.x(), vertex_pt2.y()), Point(skeleton_pt.x(), skeleton_pt.y())));
                    if (has_skeleton_pt2)
                        helper_lines.emplace_back(Line(Point(vertex_pt.x(), vertex_pt.y()), Point(skeleton_pt2.x(), skeleton_pt2.y())));
                }
            }
        dump_voronoi_to_svg(debug_out_path("voronoi-skeleton-edges-%d.svg", irun).c_str(), vd, Points(), lines, Polygons(), helper_lines);
    }
#endif // VORONOI_DEBUG_OUT

    return out;
}

} // namespace Voronoi
} // namespace Slic3r
