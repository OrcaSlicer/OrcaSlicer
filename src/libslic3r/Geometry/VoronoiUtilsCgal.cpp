// Needed since the CGAL headers are not self-contained.
#include <boost/next_prior.hpp>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Arr_segment_traits_2.h>
#include <CGAL/Surface_sweep_2_algorithms.h>
#include <boost/variant/get.hpp>
#include <vector>
#include <cassert>

#include "libslic3r/Geometry/Voronoi.hpp"
#include "libslic3r/Geometry/VoronoiUtils.hpp"
#include "libslic3r/Arachne/utils/PolygonsSegmentIndex.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "VoronoiUtilsCgal.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"

namespace CGAL {
class MP_Float;
}  // namespace CGAL

using VD = Slic3r::Geometry::VoronoiDiagram;

namespace Slic3r::Geometry {

using PolygonsSegmentIndexConstIt = std::vector<Arachne::PolygonsSegmentIndex>::const_iterator;
using LinesIt                     = Lines::iterator;
using ColoredLinesConstIt         = ColoredLines::const_iterator;

// Explicit template instantiation.
template bool VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(const VD &, LinesIt, LinesIt);
template bool VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(const VD &, VD::SegmentIt, VD::SegmentIt);
template bool VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(const VD &, ColoredLinesConstIt, ColoredLinesConstIt);
template bool VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(const VD &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);

// The tangent vector of the parabola is computed based on the Proof of the reflective property.
// https://en.wikipedia.org/wiki/Parabola#Proof_of_the_reflective_property
// https://math.stackexchange.com/q/2439647/2439663#comment5039739_2439663
namespace impl {
    using K   = CGAL::Simple_cartesian<double>;
    using FK  = CGAL::Simple_cartesian<CGAL::Interval_nt_advanced>;
    using EK  = CGAL::Simple_cartesian<CGAL::MP_Float>;
    using C2E = CGAL::Cartesian_converter<K, EK>;
    using C2F = CGAL::Cartesian_converter<K, FK>;
    class Epick : public CGAL::Filtered_kernel_adaptor<CGAL::Type_equality_wrapper<K::Base<Epick>::Type, Epick>, true> {};

    template<typename K>
    inline typename K::Vector_2 calculate_parabolic_tangent_vector(
        // Test point on the parabola, where the tangent will be calculated.
        const typename K::Point_2 &p,
        // Focus point of the parabola.
        const typename K::Point_2 &f,
        // Points of a directrix of the parabola.
        const typename K::Point_2 &u,
        const typename K::Point_2 &v,
        // On which side of the parabolic segment endpoints the focus point is, which determines the orientation of the tangent.
        const typename K::Orientation &tangent_orientation)
    {
        using RT       = typename K::RT;
        using Vector_2 = typename K::Vector_2;

        const Vector_2 directrix_vec            = v - u;
        const RT       directrix_vec_sqr_length = CGAL::scalar_product(directrix_vec, directrix_vec);
        Vector_2       focus_vec                = (f - u) * directrix_vec_sqr_length - directrix_vec * CGAL::scalar_product(directrix_vec, p - u);
        Vector_2       tangent_vec              = focus_vec.perpendicular(tangent_orientation);
        return tangent_vec;
    }

    template<typename K> struct ParabolicTangentToSegmentOrientationPredicate
    {
        using Point_2     = typename K::Point_2;
        using Vector_2    = typename K::Vector_2;
        using Orientation = typename K::Orientation;
        using result_type = typename K::Orientation;

        result_type operator()(
            // Test point on the parabola, where the tangent will be calculated.
            const Point_2 &p,
            // End of the linear segment (p, q), for which orientation towards the tangent to parabola will be evaluated.
            const Point_2 &q,
            // Focus point of the parabola.
            const Point_2 &f,
            // Points of a directrix of the parabola.
            const Point_2 &u,
            const Point_2 &v,
            // On which side of the parabolic segment endpoints the focus point is, which determines the orientation of the tangent.
            const Orientation &tangent_orientation) const
        {
            assert(tangent_orientation == CGAL::Orientation::LEFT_TURN || tangent_orientation == CGAL::Orientation::RIGHT_TURN);

            Vector_2 tangent_vec = calculate_parabolic_tangent_vector<K>(p, f, u, v, tangent_orientation);
            Vector_2 linear_vec  = q - p;

            return CGAL::sign(tangent_vec.x() * linear_vec.y() - tangent_vec.y() * linear_vec.x());
        }
    };

    template<typename K> struct ParabolicTangentToParabolicTangentOrientationPredicate
    {
        using Point_2     = typename K::Point_2;
        using Vector_2    = typename K::Vector_2;
        using Orientation = typename K::Orientation;
        using result_type = typename K::Orientation;

        result_type operator()(
            // Common point on both parabolas, where the tangent will be calculated.
            const Point_2 &p,
            // Focus point of the first parabola.
            const Point_2 &f_0,
            // Points of a directrix of the first parabola.
            const Point_2 &u_0,
            const Point_2 &v_0,
            // On which side of the parabolic segment endpoints the focus point is, which determines the orientation of the tangent.
            const Orientation &tangent_orientation_0,
            // Focus point of the second parabola.
            const Point_2 &f_1,
            // Points of a directrix of the second parabola.
            const Point_2 &u_1,
            const Point_2 &v_1,
            // On which side of the parabolic segment endpoints the focus point is, which determines the orientation of the tangent.
            const Orientation &tangent_orientation_1) const
        {
            assert(tangent_orientation_0 == CGAL::Orientation::LEFT_TURN || tangent_orientation_0 == CGAL::Orientation::RIGHT_TURN);
            assert(tangent_orientation_1 == CGAL::Orientation::LEFT_TURN || tangent_orientation_1 == CGAL::Orientation::RIGHT_TURN);

            Vector_2 tangent_vec_0 = calculate_parabolic_tangent_vector<K>(p, f_0, u_0, v_0, tangent_orientation_0);
            Vector_2 tangent_vec_1 = calculate_parabolic_tangent_vector<K>(p, f_1, u_1, v_1, tangent_orientation_1);

            return CGAL::sign(tangent_vec_0.x() * tangent_vec_1.y() - tangent_vec_0.y() * tangent_vec_1.x());
        }
    };

    using ParabolicTangentToSegmentOrientationPredicateFiltered = CGAL::Filtered_predicate<ParabolicTangentToSegmentOrientationPredicate<EK>, ParabolicTangentToSegmentOrientationPredicate<FK>, C2E, C2F>;
    using ParabolicTangentToParabolicTangentOrientationPredicateFiltered = CGAL::Filtered_predicate<ParabolicTangentToParabolicTangentOrientationPredicate<EK>, ParabolicTangentToParabolicTangentOrientationPredicate<FK>, C2E, C2F>;
} // namespace impl

using ParabolicTangentToSegmentOrientation = impl::ParabolicTangentToSegmentOrientationPredicateFiltered;
using ParabolicTangentToParabolicTangentOrientation = impl::ParabolicTangentToParabolicTangentOrientationPredicateFiltered;
using CGAL_Point   = impl::K::Point_2;

inline CGAL_Point to_cgal_point(const VD::vertex_type *pt) { return {pt->x(), pt->y()}; }
inline CGAL_Point to_cgal_point(const Point &pt) { return {pt.x(), pt.y()}; }
inline CGAL_Point to_cgal_point(const Vec2d &pt) { return {pt.x(), pt.y()}; }

inline Linef make_linef(const VD::edge_type &edge)
{
    const VD::vertex_type *v0 = edge.vertex0();
    const VD::vertex_type *v1 = edge.vertex1();
    return {Vec2d(v0->x(), v0->y()), Vec2d(v1->x(), v1->y())};
}

[[maybe_unused]] inline bool is_equal(const VD::vertex_type &vertex_first, const VD::vertex_type &vertex_second) { return vertex_first.x() == vertex_second.x() && vertex_first.y() == vertex_second.y(); }

// FIXME Lukas H.: Also includes parabolic segments.
bool VoronoiUtilsCgal::is_voronoi_diagram_planar_intersection(const VD &voronoi_diagram)
{
    using CGAL_E_Point   = CGAL::Exact_predicates_exact_constructions_kernel::Point_2;
    using CGAL_E_Segment = CGAL::Arr_segment_traits_2<CGAL::Exact_predicates_exact_constructions_kernel>::Curve_2;
    auto to_cgal_point   = [](const VD::vertex_type &pt) -> CGAL_E_Point { return {pt.x(), pt.y()}; };

    assert(std::all_of(voronoi_diagram.edges().cbegin(), voronoi_diagram.edges().cend(),
                       [](const VD::edge_type &edge) { return edge.color() == 0; }));

    std::vector<CGAL_E_Segment> segments;
    segments.reserve(voronoi_diagram.num_edges());

    for (const VD::edge_type &edge : voronoi_diagram.edges()) {
        if (edge.color() != 0)
            continue;

        if (edge.is_finite() && edge.is_linear() && edge.vertex0() != nullptr && edge.vertex1() != nullptr &&
            VoronoiUtils::is_finite(*edge.vertex0()) && VoronoiUtils::is_finite(*edge.vertex1())) {
            segments.emplace_back(to_cgal_point(*edge.vertex0()), to_cgal_point(*edge.vertex1()));
            edge.color(1);
            assert(edge.twin() != nullptr);
            edge.twin()->color(1);
        }
    }

    for (const VD::edge_type &edge : voronoi_diagram.edges())
        edge.color(0);

    std::vector<CGAL_E_Point> intersections_pt;
    CGAL::compute_intersection_points(segments.begin(), segments.end(), std::back_inserter(intersections_pt));
    return intersections_pt.empty();
}

struct ParabolicSegment
{
    const Point focus;
    const Line  directrix;
    // Two points on the parabola;
    const Linef segment;
    // Indicate if focus point is on the left side or right side relative to parabolic segment endpoints.
    const CGAL::Orientation is_focus_on_left;
};

template<typename SegmentIterator>
inline static typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    ParabolicSegment>::type
get_parabolic_segment(const VD::edge_type &edge, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    using Segment = typename std::iterator_traits<SegmentIterator>::value_type;
    assert(edge.is_curved());

    const VD::cell_type *left_cell  = edge.cell();
    const VD::cell_type *right_cell = edge.twin()->cell();

    const Point       focus_pt   = VoronoiUtils::get_source_point(*(left_cell->contains_point() ? left_cell : right_cell), segment_begin, segment_end);
    const Segment    &directrix  = VoronoiUtils::get_source_segment(*(left_cell->contains_point() ? right_cell : left_cell), segment_begin, segment_end);
    CGAL::Orientation focus_side = CGAL::opposite(CGAL::orientation(to_cgal_point(edge.vertex0()), to_cgal_point(edge.vertex1()), to_cgal_point(focus_pt)));

    assert(focus_side == CGAL::Orientation::LEFT_TURN || focus_side == CGAL::Orientation::RIGHT_TURN);

    const Point directrix_from = boost::polygon::segment_traits<Segment>::get(directrix, boost::polygon::LOW);
    const Point directrix_to   = boost::polygon::segment_traits<Segment>::get(directrix, boost::polygon::HIGH);
    return {focus_pt, Line(directrix_from, directrix_to), make_linef(edge), focus_side};
}

template<typename SegmentIterator>
inline static typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    CGAL::Orientation>::type
orientation_of_two_edges(const VD::edge_type &edge_a, const VD::edge_type &edge_b, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    assert(is_equal(*edge_a.vertex0(), *edge_b.vertex0()));
    CGAL::Orientation orientation;
    if (edge_a.is_linear() && edge_b.is_linear()) {
        orientation = CGAL::orientation(to_cgal_point(edge_a.vertex0()), to_cgal_point(edge_a.vertex1()), to_cgal_point(edge_b.vertex1()));
    } else if (edge_a.is_curved() && edge_b.is_curved()) {
        const ParabolicSegment parabolic_a = get_parabolic_segment(edge_a, segment_begin, segment_end);
        const ParabolicSegment parabolic_b = get_parabolic_segment(edge_b, segment_begin, segment_end);
        orientation = ParabolicTangentToParabolicTangentOrientation{}(to_cgal_point(parabolic_a.segment.a),
                                                                      to_cgal_point(parabolic_a.focus),
                                                                      to_cgal_point(parabolic_a.directrix.a),
                                                                      to_cgal_point(parabolic_a.directrix.b),
                                                                      parabolic_a.is_focus_on_left,
                                                                      to_cgal_point(parabolic_b.focus),
                                                                      to_cgal_point(parabolic_b.directrix.a),
                                                                      to_cgal_point(parabolic_b.directrix.b),
                                                                      parabolic_b.is_focus_on_left);
        return orientation;
    } else {
        assert(edge_a.is_curved() != edge_b.is_curved());

        const VD::edge_type   &linear_edge    = edge_a.is_curved() ? edge_b : edge_a;
        const VD::edge_type   &parabolic_edge = edge_a.is_curved() ? edge_a : edge_b;
        const ParabolicSegment parabolic      = get_parabolic_segment(parabolic_edge, segment_begin, segment_end);
        orientation = ParabolicTangentToSegmentOrientation{}(to_cgal_point(parabolic.segment.a), to_cgal_point(linear_edge.vertex1()),
                                                             to_cgal_point(parabolic.focus),
                                                             to_cgal_point(parabolic.directrix.a),
                                                             to_cgal_point(parabolic.directrix.b),
                                                             parabolic.is_focus_on_left);

        if (edge_b.is_curved())
            orientation = CGAL::opposite(orientation);
    }

    return orientation;
}

template<typename SegmentIterator>
static typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    bool>::type
check_if_three_edges_are_ccw(const VD::edge_type  &edge_first,
                             const VD::edge_type  &edge_second,
                             const VD::edge_type  &edge_third,
                             const SegmentIterator segment_begin,
                             const SegmentIterator segment_end)
{
    assert(is_equal(*edge_first.vertex0(), *edge_second.vertex0()) && is_equal(*edge_second.vertex0(), *edge_third.vertex0()));

    CGAL::Orientation orientation = orientation_of_two_edges(edge_first, edge_second, segment_begin, segment_end);
    if (orientation == CGAL::Orientation::COLLINEAR) {
        // The first two edges are collinear, so the third edge must be on the right side on the first of them.
        return orientation_of_two_edges(edge_first, edge_third, segment_begin, segment_end) == CGAL::Orientation::RIGHT_TURN;
    } else if (orientation == CGAL::Orientation::LEFT_TURN) {
        // CCW oriented angle between vectors (common_pt, pt1) and (common_pt, pt2) is bellow PI.
        // So we need to check if test_pt isn't between them.
        CGAL::Orientation orientation1 = orientation_of_two_edges(edge_first, edge_third, segment_begin, segment_end);
        CGAL::Orientation orientation2 = orientation_of_two_edges(edge_second, edge_third, segment_begin, segment_end);
        return (orientation1 != CGAL::Orientation::LEFT_TURN || orientation2 != CGAL::Orientation::RIGHT_TURN);
    } else {
        assert(orientation == CGAL::Orientation::RIGHT_TURN);
        // CCW oriented angle between vectors (common_pt, pt1) and (common_pt, pt2) is upper PI.
        // So we need to check if test_pt is between them.
        CGAL::Orientation orientation1 = orientation_of_two_edges(edge_first, edge_third, segment_begin, segment_end);
        CGAL::Orientation orientation2 = orientation_of_two_edges(edge_second, edge_third, segment_begin, segment_end);
        return (orientation1 == CGAL::Orientation::RIGHT_TURN || orientation2 == CGAL::Orientation::LEFT_TURN);
    }
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    bool>::type
VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(const VD             &voronoi_diagram,
                                                  const SegmentIterator segment_begin,
                                                  const SegmentIterator segment_end)
{
    for (const VD::vertex_type &vertex : voronoi_diagram.vertices()) {
        std::vector<const VD::edge_type *> edges;
        const VD::edge_type               *edge = vertex.incident_edge();

        do {
            if (edge->is_finite() && edge->vertex0() != nullptr && edge->vertex1() != nullptr && VoronoiUtils::is_finite(*edge->vertex0()) && VoronoiUtils::is_finite(*edge->vertex1()))
                edges.emplace_back(edge);

            edge = edge->rot_next();
        } while (edge != vertex.incident_edge());

        // Checking for CCW make sense for three and more edges.
        if (edges.size() > 2) {
            for (auto edge_it = edges.begin() ; edge_it != edges.end(); ++edge_it) {
                const VD::edge_type *prev_edge = edge_it == edges.begin() ? edges.back() : *std::prev(edge_it);
                const VD::edge_type *curr_edge = *edge_it;
                const VD::edge_type *next_edge = std::next(edge_it) == edges.end() ? edges.front() : *std::next(edge_it);

                if (!check_if_three_edges_are_ccw(*prev_edge, *curr_edge, *next_edge, segment_begin, segment_end))
                    return false;
            }
        }
    }

    return true;
}

} // namespace Slic3r::Geometry
