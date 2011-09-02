#include <boost/log/trivial.hpp>
#include <libslic3r/Arachne/utils/PolygonsSegmentIndex.hpp>
#include <libslic3r/MultiMaterialSegmentation.hpp>
#include <libslic3r/Geometry.hpp>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>
#include <cassert>
#include <cstdlib>

#include "VoronoiUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Line.hpp"

namespace Slic3r::Geometry {

using PolygonsSegmentIndexConstIt = std::vector<Arachne::PolygonsSegmentIndex>::const_iterator;
using LinesIt                     = Lines::iterator;
using ColoredLinesIt              = ColoredLines::iterator;
using ColoredLinesConstIt         = ColoredLines::const_iterator;

// Explicit template instantiation.
template LinesIt::reference VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &, LinesIt, LinesIt);
template VD::SegmentIt::reference VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &, VD::SegmentIt, VD::SegmentIt);
template ColoredLinesIt::reference VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &, ColoredLinesIt, ColoredLinesIt);
template ColoredLinesConstIt::reference VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &, ColoredLinesConstIt, ColoredLinesConstIt);
template PolygonsSegmentIndexConstIt::reference VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);
template Point VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &, LinesIt, LinesIt);
template Point VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &, VD::SegmentIt, VD::SegmentIt);
template Point VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &, ColoredLinesIt, ColoredLinesIt);
template Point VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &, ColoredLinesConstIt, ColoredLinesConstIt);
template Point VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);
template SegmentCellRange<Point> VoronoiUtils::compute_segment_cell_range(const VoronoiDiagram::cell_type &, LinesIt, LinesIt);
template SegmentCellRange<Point> VoronoiUtils::compute_segment_cell_range(const VoronoiDiagram::cell_type &, VD::SegmentIt, VD::SegmentIt);
template SegmentCellRange<Point> VoronoiUtils::compute_segment_cell_range(const VoronoiDiagram::cell_type &, ColoredLinesConstIt, ColoredLinesConstIt);
template SegmentCellRange<Point> VoronoiUtils::compute_segment_cell_range(const VoronoiDiagram::cell_type &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);
template PointCellRange<Point> VoronoiUtils::compute_point_cell_range(const VoronoiDiagram::cell_type &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);
template Points VoronoiUtils::discretize_parabola(const Point &, const Arachne::PolygonsSegmentIndex &, const Point &, const Point &, coord_t, float);
template Arachne::PolygonsPointIndex VoronoiUtils::get_source_point_index(const VoronoiDiagram::cell_type &, PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt);

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    typename std::iterator_traits<SegmentIterator>::reference>::type
VoronoiUtils::get_source_segment(const VoronoiDiagram::cell_type &cell, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    if (!cell.contains_segment())
        throw Slic3r::InvalidArgument("Voronoi cell doesn't contain a source segment!");

    if (cell.source_index() >= size_t(std::distance(segment_begin, segment_end)))
        throw Slic3r::OutOfRange("Voronoi cell source index is out of range!");

    return *(segment_begin + cell.source_index());
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type
VoronoiUtils::get_source_point(const VoronoiDiagram::cell_type &cell, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    using Segment = typename std::iterator_traits<SegmentIterator>::value_type;

    if (!cell.contains_point())
        throw Slic3r::InvalidArgument("Voronoi cell doesn't contain a source point!");

    if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) {
        assert(int(cell.source_index()) < std::distance(segment_begin, segment_end));
        const SegmentIterator segment_it = segment_begin + cell.source_index();
        return boost::polygon::segment_traits<Segment>::get(*segment_it, boost::polygon::LOW);
    } else if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_END_POINT) {
        assert(int(cell.source_index()) < std::distance(segment_begin, segment_end));
        const SegmentIterator segment_it = segment_begin + cell.source_index();
        return boost::polygon::segment_traits<Segment>::get(*segment_it, boost::polygon::HIGH);
    } else if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SINGLE_POINT) {
        throw Slic3r::RuntimeError("Voronoi diagram is always constructed using segments, so cell.source_category() shouldn't be SOURCE_CATEGORY_SINGLE_POINT!");
    } else {
        throw Slic3r::InvalidArgument("Function get_source_point() should only be called on point cells!");
    }
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    Arachne::PolygonsPointIndex>::type
VoronoiUtils::get_source_point_index(const VD::cell_type &cell, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    if (!cell.contains_point())
        throw Slic3r::InvalidArgument("Voronoi cell doesn't contain a source point!");

    if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) {
        assert(int(cell.source_index()) < std::distance(segment_begin, segment_end));
        const SegmentIterator segment_it = segment_begin + cell.source_index();
        return (*segment_it);
    } else if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_END_POINT) {
        assert(int(cell.source_index()) < std::distance(segment_begin, segment_end));
        const SegmentIterator segment_it = segment_begin + cell.source_index();
        return (*segment_it).next();
    } else if (cell.source_category() == boost::polygon::SOURCE_CATEGORY_SINGLE_POINT) {
        throw Slic3r::RuntimeError("Voronoi diagram is always constructed using segments, so cell.source_category() shouldn't be SOURCE_CATEGORY_SINGLE_POINT!");
    } else {
        throw Slic3r::InvalidArgument("Function get_source_point_index() should only be called on point cells!");
    }
}

template<typename Segment>
typename boost::polygon::enable_if<typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
    typename boost::polygon::geometry_concept<Segment>::type>::type>::type,
    Points>::type
VoronoiUtils::discretize_parabola(const Point &source_point, const Segment &source_segment, const Point &start, const Point &end, const coord_t approximate_step_size, float transitioning_angle)
{
    Points discretized;
    // x is distance of point projected on the segment ab
    // xx is point projected on the segment ab
    const Point   a       = source_segment.from();
    const Point   b       = source_segment.to();
    const Point   ab      = b - a;
    const Point   as      = start - a;
    const Point   ae      = end - a;
    const coord_t ab_size = ab.cast<int64_t>().norm();
    const coord_t sx      = as.cast<int64_t>().dot(ab.cast<int64_t>()) / ab_size;
    const coord_t ex      = ae.cast<int64_t>().dot(ab.cast<int64_t>()) / ab_size;
    const coord_t sxex    = ex - sx;

    const Point   ap = source_point - a;
    const coord_t px = ap.cast<int64_t>().dot(ab.cast<int64_t>()) / ab_size;

    Point pxx;
    Line(a, b).distance_to_infinite_squared(source_point, &pxx);
    const Point   ppxx = pxx - source_point;
    const coord_t d    = ppxx.cast<int64_t>().norm();

    const Vec2d  rot           = perp(ppxx).cast<double>().normalized();
    const double rot_cos_theta = rot.x();
    const double rot_sin_theta = rot.y();

    if (d == 0) {
        discretized.emplace_back(start);
        discretized.emplace_back(end);
        return discretized;
    }

    const double marking_bound = atan(transitioning_angle * 0.5);
    int64_t      msx           = -marking_bound * int64_t(d); // projected marking_start
    int64_t      mex           = marking_bound * int64_t(d);  // projected marking_end

    const coord_t marking_start_end_h = msx * msx / (2 * d) + d / 2;
    Point         marking_start       = Point(coord_t(msx), marking_start_end_h).rotated(rot_cos_theta, rot_sin_theta) + pxx;
    Point         marking_end         = Point(coord_t(mex), marking_start_end_h).rotated(rot_cos_theta, rot_sin_theta) + pxx;
    const int     dir                 = (sx > ex) ? -1 : 1;
    if (dir < 0) {
        std::swap(marking_start, marking_end);
        std::swap(msx, mex);
    }

    bool add_marking_start = msx * int64_t(dir) > int64_t(sx - px) * int64_t(dir) && msx * int64_t(dir) < int64_t(ex - px) * int64_t(dir);
    bool add_marking_end   = mex * int64_t(dir) > int64_t(sx - px) * int64_t(dir) && mex * int64_t(dir) < int64_t(ex - px) * int64_t(dir);

    const Point apex     = Point(0, d / 2).rotated(rot_cos_theta, rot_sin_theta) + pxx;
    bool        add_apex = int64_t(sx - px) * int64_t(dir) < 0 && int64_t(ex - px) * int64_t(dir) > 0;

    assert(!add_marking_start || !add_marking_end || add_apex);
    if (add_marking_start && add_marking_end && !add_apex)
        BOOST_LOG_TRIVIAL(warning) << "Failing to discretize parabola! Must add an apex or one of the endpoints.";

    const coord_t step_count = lround(static_cast<double>(std::abs(ex - sx)) / approximate_step_size);
    discretized.emplace_back(start);
    for (coord_t step = 1; step < step_count; ++step) {
        const int64_t x = int64_t(sx) + int64_t(sxex) * int64_t(step) / int64_t(step_count) - int64_t(px);
        const int64_t y = int64_t(x) * int64_t(x) / int64_t(2 * d) + int64_t(d / 2);

        if (add_marking_start && msx * int64_t(dir) < int64_t(x) * int64_t(dir)) {
            discretized.emplace_back(marking_start);
            add_marking_start = false;
        }

        if (add_apex && int64_t(x) * int64_t(dir) > 0) {
            discretized.emplace_back(apex);
            add_apex = false; // only add the apex just before the
        }

        if (add_marking_end && mex * int64_t(dir) < int64_t(x) * int64_t(dir)) {
            discretized.emplace_back(marking_end);
            add_marking_end = false;
        }

        assert(is_in_range<coord_t>(x) && is_in_range<coord_t>(y));
        const Point result = Point(x, y).rotated(rot_cos_theta, rot_sin_theta) + pxx;
        discretized.emplace_back(result);
    }

    if (add_apex)
        discretized.emplace_back(apex);

    if (add_marking_end)
        discretized.emplace_back(marking_end);

    discretized.emplace_back(end);
    return discretized;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    Geometry::SegmentCellRange<
        typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>>::type
VoronoiUtils::compute_segment_cell_range(const VD::cell_type &cell, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    using Segment          = typename std::iterator_traits<SegmentIterator>::value_type;
    using Point            = typename boost::polygon::segment_point_type<Segment>::type;
    using SegmentCellRange = SegmentCellRange<Point>;

    const Segment &source_segment = Geometry::VoronoiUtils::get_source_segment(cell, segment_begin, segment_end);
    const Point    from           = boost::polygon::segment_traits<Segment>::get(source_segment, boost::polygon::LOW);
    const Point    to             = boost::polygon::segment_traits<Segment>::get(source_segment, boost::polygon::HIGH);
    const Vec2i64  from_i64       = from.template cast<int64_t>();
    const Vec2i64  to_i64         = to.template cast<int64_t>();

    // FIXME @hejllukas: Ensure that there is no infinite edge during iteration between edge_begin and edge_end.
    SegmentCellRange cell_range(to, from);

    // Find starting edge and end edge
    bool                 seen_possible_start             = false;
    bool                 after_start                     = false;
    bool                 ending_edge_is_set_before_start = false;
    const VD::edge_type *edge                            = cell.incident_edge();
    do {
        if (edge->is_infinite())
            continue;

        Vec2i64 v0 = Geometry::VoronoiUtils::to_point(edge->vertex0());
        Vec2i64 v1 = Geometry::VoronoiUtils::to_point(edge->vertex1());
        assert(v0 != to_i64 || v1 != from_i64);

        if (v0 == to_i64 && !after_start) { // Use the last edge which starts in source_segment.to
            cell_range.edge_begin = edge;
            seen_possible_start   = true;
        } else if (seen_possible_start) {
            after_start = true;
        }

        if (v1 == from_i64 && (!cell_range.edge_end || ending_edge_is_set_before_start)) {
            ending_edge_is_set_before_start = !after_start;
            cell_range.edge_end             = edge;
        }
    } while (edge = edge->next(), edge != cell.incident_edge());

    return cell_range;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    Geometry::PointCellRange<
        typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>>::type
VoronoiUtils::compute_point_cell_range(const VD::cell_type &cell, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    using Segment        = typename std::iterator_traits<SegmentIterator>::value_type;
    using Point          = typename boost::polygon::segment_point_type<Segment>::type;
    using PointCellRange = PointCellRange<Point>;
    using CoordType      = typename Point::coord_type;

    const Point source_point = Geometry::VoronoiUtils::get_source_point(cell, segment_begin, segment_end);

    // We want to ignore (by returning PointCellRange without assigned edge_begin and edge_end) cells outside the input polygon.
    PointCellRange cell_range(source_point);

    const VD::edge_type *edge = cell.incident_edge();
    if (edge->is_infinite() || !is_in_range<CoordType>(*edge)) {
        // Ignore infinite edges, because they only occur outside the polygon.
        // Also ignore edges with endpoints that don't fit into CoordType, because such edges are definitely outside the polygon.
        return cell_range;
    }

    const Arachne::PolygonsPointIndex source_point_idx = Geometry::VoronoiUtils::get_source_point_index(cell, segment_begin, segment_end);
    const Point                       edge_v0          = Geometry::VoronoiUtils::to_point(edge->vertex0()).template cast<CoordType>();
    const Point                       edge_v1          = Geometry::VoronoiUtils::to_point(edge->vertex1()).template cast<CoordType>();
    const Point                       edge_query_point = (edge_v0 == source_point) ? edge_v1 : edge_v0;

    // Check if the edge has another endpoint inside the corner of the polygon.
    if (!Geometry::is_point_inside_polygon_corner(source_point_idx.prev().p(), source_point_idx.p(), source_point_idx.next().p(), edge_query_point)) {
        // If the endpoint isn't inside the corner of the polygon, it means that
        // the whole cell isn't inside the polygons, and we will ignore such cells.
        return cell_range;
    }

    const Vec2i64 source_point_i64 = source_point.template cast<int64_t>();
    edge = cell.incident_edge();
    do {
        assert(edge->is_finite());

        if (Vec2i64 v1 = Geometry::VoronoiUtils::to_point(edge->vertex1()); v1 == source_point_i64) {
            cell_range.edge_begin = edge->next();
            cell_range.edge_end   = edge;
        } else {
            // FIXME @hejllukas: With Arachne, we don't support polygons with collinear edges,
            //                   because with collinear edges we have to handle secondary edges.
            //                   Such edges goes through the endpoints of the input segments.
            assert((Geometry::VoronoiUtils::to_point(edge->vertex0()) == source_point_i64 || edge->is_primary()) && "Point cells must end in the point! They cannot cross the point with an edge, because collinear edges are not allowed in the input.");
        }
    } while (edge = edge->next(), edge != cell.incident_edge());

    return cell_range;
}

Vec2i64 VoronoiUtils::to_point(const VD::vertex_type *vertex)
{
    assert(vertex != nullptr);
    return VoronoiUtils::to_point(*vertex);
}

Vec2i64 VoronoiUtils::to_point(const VD::vertex_type &vertex)
{
    const double x = vertex.x(), y = vertex.y();

    assert(std::isfinite(x) && std::isfinite(y));
    assert(is_in_range<int64_t>(x) && is_in_range<int64_t>(y));

    return {std::llround(x), std::llround(y)};
}

bool VoronoiUtils::is_finite(const VD::vertex_type &vertex)
{
    return std::isfinite(vertex.x()) && std::isfinite(vertex.y());
}

VD::vertex_type VoronoiUtils::make_rotated_vertex(VD::vertex_type &vertex, const double angle)
{
    const double cos_a = std::cos(angle);
    const double sin_a = std::sin(angle);

    const double rotated_x = (cos_a * vertex.x() - sin_a * vertex.y());
    const double rotated_y = (cos_a * vertex.y() + sin_a * vertex.x());

    VD::vertex_type rotated_vertex{rotated_x, rotated_y};
    rotated_vertex.incident_edge(vertex.incident_edge());
    rotated_vertex.color(vertex.color());

    return rotated_vertex;
}

} // namespace Slic3r::Geometry
