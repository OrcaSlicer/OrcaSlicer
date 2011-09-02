#ifndef slic3r_VoronoiUtils_hpp_
#define slic3r_VoronoiUtils_hpp_

#include <boost/polygon/polygon.hpp>
#include <iterator>
#include <limits>

#include "libslic3r/Geometry/Voronoi.hpp"
#include "libslic3r/Arachne/utils/PolygonsSegmentIndex.hpp"
#include "libslic3r/Arachne/utils/PolygonsPointIndex.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

using VD = Slic3r::Geometry::VoronoiDiagram;

namespace Slic3r::Geometry {

// Represent trapezoid Voronoi cell around segment.
template<typename PT> struct SegmentCellRange
{
    const PT             source_segment_start_point; // The start point of the source segment of this cell.
    const PT             source_segment_end_point;   // The end point of the source segment of this cell.
    const VD::edge_type *edge_begin = nullptr;       // The edge of the Voronoi diagram where the loop around the cell starts.
    const VD::edge_type *edge_end   = nullptr;       // The edge of the Voronoi diagram where the loop around the cell ends.

    SegmentCellRange() = delete;
    explicit SegmentCellRange(const PT &source_segment_start_point, const PT &source_segment_end_point)
        : source_segment_start_point(source_segment_start_point), source_segment_end_point(source_segment_end_point)
    {}

    bool is_valid() const { return edge_begin && edge_end && edge_begin != edge_end; }
};

// Represent trapezoid Voronoi cell around point.
template<typename PT> struct PointCellRange
{
    const PT             source_point;  // The source point of this cell.
    const VD::edge_type *edge_begin = nullptr; // The edge of the Voronoi diagram where the loop around the cell starts.
    const VD::edge_type *edge_end   = nullptr; // The edge of the Voronoi diagram where the loop around the cell ends.

    PointCellRange() = delete;
    explicit PointCellRange(const PT &source_point) : source_point(source_point) {}

    bool is_valid() const { return edge_begin && edge_end && edge_begin != edge_end; }
};

class VoronoiUtils
{
public:
    static Vec2i64 to_point(const VD::vertex_type *vertex);

    static Vec2i64 to_point(const VD::vertex_type &vertex);

    static bool is_finite(const VD::vertex_type &vertex);

    static VD::vertex_type make_rotated_vertex(VD::vertex_type &vertex, double angle);

    template<typename SegmentIterator>
    static typename boost::polygon::enable_if<
        typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
            typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
        typename std::iterator_traits<SegmentIterator>::reference>::type
    get_source_segment(const VD::cell_type &cell, SegmentIterator segment_begin, SegmentIterator segment_end);

    template<typename SegmentIterator>
    static typename boost::polygon::enable_if<
        typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
            typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
        typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type
    get_source_point(const VoronoiDiagram::cell_type &cell, SegmentIterator segment_begin, SegmentIterator segment_end);

    template<typename SegmentIterator>
    static typename boost::polygon::enable_if<
        typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
            typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
        Arachne::PolygonsPointIndex>::type
    get_source_point_index(const VD::cell_type &cell, SegmentIterator segment_begin, SegmentIterator segment_end);

    /**
     * Discretize a parabola based on (approximate) step size.
     *
     * Adapted from CuraEngine VoronoiUtils::discretizeParabola by Tim Kuipers @BagelOrb and @Ghostkeeper.
     *
     * @param approximate_step_size is measured parallel to the source_segment, not along the parabola.
     */
    template<typename Segment>
    static typename boost::polygon::enable_if<typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<Segment>::type>::type>::type,
        Points>::type
    discretize_parabola(const Point &source_point, const Segment &source_segment, const Point &start, const Point &end, coord_t approximate_step_size, float transitioning_angle);

    /**
     * Compute the range of line segments that surround a cell of the skeletal
     * graph that belongs to a line segment of the medial axis.
     *
     * This should only be used on cells that belong to a central line segment
     * of the skeletal graph, e.g. trapezoid cells, not triangular cells.
     *
     * The resulting line segments is just the first and the last segment. They
     * are linked to the neighboring segments, so you can iterate over the
     * segments until you reach the last segment.
     *
     * Adapted from CuraEngine VoronoiUtils::computeSegmentCellRange by Tim Kuipers @BagelOrb,
     * Jaime van Kessel @nallath, Remco Burema @rburema and @Ghostkeeper.
     *
     * @param cell The cell to compute the range of line segments for.
     * @param segment_begin Begin iterator for all edges of the input Polygons.
     * @param segment_end End iterator for all edges of the input Polygons.
     * @return Range of line segments that surround the cell.
     */
    template<typename SegmentIterator>
    static typename boost::polygon::enable_if<
        typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
            typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
        Geometry::SegmentCellRange<
            typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>>::type
    compute_segment_cell_range(const VD::cell_type &cell, SegmentIterator segment_begin, SegmentIterator segment_end);

    /**
     * Compute the range of line segments that surround a cell of the skeletal
     * graph that belongs to a point on the medial axis.
     *
     * This should only be used on cells that belong to a corner in the skeletal
     * graph, e.g. triangular cells, not trapezoid cells.
     *
     * The resulting line segments is just the first and the last segment. They
     * are linked to the neighboring segments, so you can iterate over the
     * segments until you reach the last segment.
     *
     * Adapted from CuraEngine VoronoiUtils::computePointCellRange by Tim Kuipers @BagelOrb
     * Jaime van Kessel @nallath, Remco Burema @rburema and @Ghostkeeper.
     *
     * @param cell The cell to compute the range of line segments for.
     * @param segment_begin Begin iterator for all edges of the input Polygons.
     * @param segment_end End iterator for all edges of the input Polygons.
     * @return Range of line segments that surround the cell.
     */
    template<typename SegmentIterator>
    static typename boost::polygon::enable_if<
        typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
            typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
        Geometry::PointCellRange<
            typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>>::type
    compute_point_cell_range(const VD::cell_type &cell, SegmentIterator segment_begin, SegmentIterator segment_end);

    template<typename T> static bool is_in_range(double value)
    {
        return double(std::numeric_limits<T>::lowest()) <= value && value <= double(std::numeric_limits<T>::max());
    }

    template<typename T> static bool is_in_range(const VD::vertex_type &vertex)
    {
        return VoronoiUtils::is_finite(vertex) && is_in_range<T>(vertex.x()) && is_in_range<T>(vertex.y());
    }

    template<typename T> static bool is_in_range(const VD::edge_type &edge)
    {
        if (edge.vertex0() == nullptr || edge.vertex1() == nullptr)
            return false;

        return is_in_range<T>(*edge.vertex0()) && is_in_range<T>(*edge.vertex1());
    }
};

} // namespace Slic3r::Geometry

#endif // slic3r_VoronoiUtils_hpp_
