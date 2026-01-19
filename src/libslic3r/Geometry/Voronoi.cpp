#include "Voronoi.hpp"

#include <boost/log/trivial.hpp>
#include <cassert>

#include "libslic3r/Arachne/utils/PolygonsSegmentIndex.hpp"
#include "libslic3r/Geometry/VoronoiUtils.hpp"
#include "libslic3r/Geometry/VoronoiUtilsCgal.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Line.hpp"

namespace Slic3r::Geometry {

using PolygonsSegmentIndexConstIt = std::vector<Arachne::PolygonsSegmentIndex>::const_iterator;
using LinesIt                     = Lines::iterator;
using ColoredLinesConstIt         = ColoredLines::const_iterator;

// Explicit template instantiation.
template void VoronoiDiagram::construct_voronoi(LinesIt, LinesIt, bool);
template void VoronoiDiagram::construct_voronoi(ColoredLinesConstIt, ColoredLinesConstIt, bool);
template void VoronoiDiagram::construct_voronoi(PolygonsSegmentIndexConstIt, PolygonsSegmentIndexConstIt, bool);

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    void>::type
VoronoiDiagram::construct_voronoi(const SegmentIterator segment_begin, const SegmentIterator segment_end, const bool try_to_repair_if_needed) {
    boost::polygon::construct_voronoi(segment_begin, segment_end, &m_voronoi_diagram);
    if (try_to_repair_if_needed) {
        if (m_issue_type = detect_known_issues(*this, segment_begin, segment_end); m_issue_type != IssueType::NO_ISSUE_DETECTED) {
            if (m_issue_type == IssueType::MISSING_VORONOI_VERTEX) {
                BOOST_LOG_TRIVIAL(warning) << "Detected missing Voronoi vertex, input polygons will be rotated back and forth.";
            } else if (m_issue_type == IssueType::NON_PLANAR_VORONOI_DIAGRAM) {
                BOOST_LOG_TRIVIAL(warning) << "Detected non-planar Voronoi diagram, input polygons will be rotated back and forth.";
            } else if (m_issue_type == IssueType::VORONOI_EDGE_INTERSECTING_INPUT_SEGMENT) {
                BOOST_LOG_TRIVIAL(warning) << "Detected Voronoi edge intersecting input segment, input polygons will be rotated back and forth.";
            } else if (m_issue_type == IssueType::FINITE_EDGE_WITH_NON_FINITE_VERTEX) {
                BOOST_LOG_TRIVIAL(warning) << "Detected finite Voronoi vertex with non finite vertex, input polygons will be rotated back and forth.";
            } else if (m_issue_type == IssueType::PARABOLIC_VORONOI_EDGE_WITHOUT_FOCUS_POINT) {
                BOOST_LOG_TRIVIAL(warning) << "Detected parabolic Voronoi edges without focus point, input polygons will be rotated back and forth.";
            } else {
                BOOST_LOG_TRIVIAL(error) << "Detected unknown Voronoi diagram issue, input polygons will be rotated back and forth.";
            }

            if (m_issue_type = try_to_repair_degenerated_voronoi_diagram(segment_begin, segment_end); m_issue_type != IssueType::NO_ISSUE_DETECTED) {
                if (m_issue_type == IssueType::MISSING_VORONOI_VERTEX) {
                    BOOST_LOG_TRIVIAL(error) << "Detected missing Voronoi vertex even after the rotation of input.";
                } else if (m_issue_type == IssueType::NON_PLANAR_VORONOI_DIAGRAM) {
                    BOOST_LOG_TRIVIAL(error) << "Detected non-planar Voronoi diagram even after the rotation of input.";
                } else if (m_issue_type == IssueType::VORONOI_EDGE_INTERSECTING_INPUT_SEGMENT) {
                    BOOST_LOG_TRIVIAL(error) << "Detected Voronoi edge intersecting input segment even after the rotation of input.";
                } else if (m_issue_type == IssueType::FINITE_EDGE_WITH_NON_FINITE_VERTEX) {
                    BOOST_LOG_TRIVIAL(error) << "Detected finite Voronoi vertex with non finite vertex even after the rotation of input.";
                } else if (m_issue_type == IssueType::PARABOLIC_VORONOI_EDGE_WITHOUT_FOCUS_POINT) {
                    BOOST_LOG_TRIVIAL(error) << "Detected parabolic Voronoi edges without focus point even after the rotation of input.";
                } else {
                    BOOST_LOG_TRIVIAL(error) << "Detected unknown Voronoi diagram issue even after the rotation of input.";
                }

                m_state = State::REPAIR_UNSUCCESSFUL;
            } else {
                m_state = State::REPAIR_SUCCESSFUL;
            }
        } else {
            m_state      = State::REPAIR_NOT_NEEDED;
            m_issue_type = IssueType::NO_ISSUE_DETECTED;
        }
    } else {
        m_state      = State::UNKNOWN;
        m_issue_type = IssueType::UNKNOWN;
    }
}

void VoronoiDiagram::clear()
{
    if (m_is_modified) {
        m_vertices.clear();
        m_edges.clear();
        m_cells.clear();
        m_is_modified = false;
    } else {
        m_voronoi_diagram.clear();
    }

    m_state      = State::UNKNOWN;
    m_issue_type = IssueType::UNKNOWN;
}

void VoronoiDiagram::copy_to_local(voronoi_diagram_type &voronoi_diagram) {
    m_edges.clear();
    m_cells.clear();
    m_vertices.clear();

    // Copy Voronoi edges.
    m_edges.reserve(voronoi_diagram.num_edges());
    for (const edge_type &edge : voronoi_diagram.edges()) {
        m_edges.emplace_back(edge.is_linear(), edge.is_primary());
        m_edges.back().color(edge.color());
    }

    // Copy Voronoi cells.
    m_cells.reserve(voronoi_diagram.num_cells());
    for (const cell_type &cell : voronoi_diagram.cells()) {
        m_cells.emplace_back(cell.source_index(), cell.source_category());
        m_cells.back().color(cell.color());

        if (cell.incident_edge()) {
            size_t incident_edge_idx = cell.incident_edge() - voronoi_diagram.edges().data();
            m_cells.back().incident_edge(&m_edges[incident_edge_idx]);
        }
    }

    // Copy Voronoi vertices.
    m_vertices.reserve(voronoi_diagram.num_vertices());
    for (const vertex_type &vertex : voronoi_diagram.vertices()) {
        m_vertices.emplace_back(vertex.x(), vertex.y());
        m_vertices.back().color(vertex.color());

        if (vertex.incident_edge()) {
            size_t incident_edge_idx = vertex.incident_edge() - voronoi_diagram.edges().data();
            m_vertices.back().incident_edge(&m_edges[incident_edge_idx]);
        }
    }

    // Assign all pointers for each Voronoi edge.
    for (const edge_type &old_edge : voronoi_diagram.edges()) {
        size_t     edge_idx = &old_edge - voronoi_diagram.edges().data();
        edge_type &new_edge = m_edges[edge_idx];

        if (old_edge.cell()) {
            size_t cell_idx = old_edge.cell() - voronoi_diagram.cells().data();
            new_edge.cell(&m_cells[cell_idx]);
        }

        if (old_edge.vertex0()) {
            size_t vertex0_idx = old_edge.vertex0() - voronoi_diagram.vertices().data();
            new_edge.vertex0(&m_vertices[vertex0_idx]);
        }

        if (old_edge.twin()) {
            size_t twin_edge_idx = old_edge.twin() - voronoi_diagram.edges().data();
            new_edge.twin(&m_edges[twin_edge_idx]);
        }

        if (old_edge.next()) {
            size_t next_edge_idx = old_edge.next() - voronoi_diagram.edges().data();
            new_edge.next(&m_edges[next_edge_idx]);
        }

        if (old_edge.prev()) {
            size_t prev_edge_idx = old_edge.prev() - voronoi_diagram.edges().data();
            new_edge.prev(&m_edges[prev_edge_idx]);
        }
    }

    m_voronoi_diagram.clear();
    m_is_modified = true;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    VoronoiDiagram::IssueType>::type
VoronoiDiagram::detect_known_issues(const VoronoiDiagram &voronoi_diagram, SegmentIterator segment_begin, SegmentIterator segment_end)
{
    if (const IssueType edge_issue_type = detect_known_voronoi_edge_issues(voronoi_diagram); edge_issue_type != IssueType::NO_ISSUE_DETECTED) {
        return edge_issue_type;
    } else if (const IssueType cell_issue_type = detect_known_voronoi_cell_issues(voronoi_diagram, segment_begin, segment_end); cell_issue_type != IssueType::NO_ISSUE_DETECTED) {
        return cell_issue_type;
    } else if (!VoronoiUtilsCgal::is_voronoi_diagram_planar_angle(voronoi_diagram, segment_begin, segment_end)) {
        // Detection of non-planar Voronoi diagram detects at least GH issues #8474, #8514 and #8446.
        return IssueType::NON_PLANAR_VORONOI_DIAGRAM;
    }

    return IssueType::NO_ISSUE_DETECTED;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    VoronoiDiagram::IssueType>::type
VoronoiDiagram::detect_known_voronoi_cell_issues(const VoronoiDiagram &voronoi_diagram,
                                                 const SegmentIterator segment_begin,
                                                 const SegmentIterator segment_end)
{
    using Segment          = typename std::iterator_traits<SegmentIterator>::value_type;
    using Point            = typename boost::polygon::segment_point_type<Segment>::type;
    using SegmentCellRange = SegmentCellRange<Point>;

    for (VD::cell_type cell : voronoi_diagram.cells()) {
        if (cell.is_degenerate() || !cell.contains_segment())
            continue; // Skip degenerated cell that has no spoon. Also, skip a cell that doesn't contain a segment.

        if (const SegmentCellRange cell_range = VoronoiUtils::compute_segment_cell_range(cell, segment_begin, segment_end); cell_range.is_valid()) {
            // Detection if Voronoi edge is intersecting input segment.
            // It detects this type of issue at least in GH issues #8446, #8474 and #8514.

            const Segment &source_segment      = Geometry::VoronoiUtils::get_source_segment(cell, segment_begin, segment_end);
            const Vec2d    source_segment_from = boost::polygon::segment_traits<Segment>::get(source_segment, boost::polygon::LOW).template cast<double>();
            const Vec2d    source_segment_to   = boost::polygon::segment_traits<Segment>::get(source_segment, boost::polygon::HIGH).template cast<double>();
            const Vec2d    source_segment_vec  = source_segment_to - source_segment_from;

            // All Voronoi vertices must be on the left side of the source segment, otherwise the Voronoi diagram is invalid.
            for (const VD::edge_type *edge = cell_range.edge_begin; edge != cell_range.edge_end; edge = edge->next()) {
                if (edge->is_infinite()) {
                    // When there is a missing Voronoi vertex, we may encounter an infinite Voronoi edge.
                    // This happens, for example, in GH issue #8846.
                    return IssueType::MISSING_VORONOI_VERTEX;
                } else if (const Vec2d edge_v1(edge->vertex1()->x(), edge->vertex1()->y()); Slic3r::cross2(source_segment_vec, edge_v1 - source_segment_from) < 0) {
                    return IssueType::VORONOI_EDGE_INTERSECTING_INPUT_SEGMENT;
                }
            }
        } else {
            // When there is a missing Voronoi vertex (especially at one of the endpoints of the input segment),
            // the returned cell_range is marked as invalid.
            // It detects this type of issue at least in GH issue #8846.
            return IssueType::MISSING_VORONOI_VERTEX;
        }
    }

    return IssueType::NO_ISSUE_DETECTED;
}

VoronoiDiagram::IssueType VoronoiDiagram::detect_known_voronoi_edge_issues(const VoronoiDiagram &voronoi_diagram)
{
    for (const voronoi_diagram_type::edge_type &edge : voronoi_diagram.edges()) {
        if (edge.is_finite()) {
            assert(edge.vertex0() != nullptr && edge.vertex1() != nullptr);
            if (edge.vertex0() == nullptr || edge.vertex1() == nullptr || !VoronoiUtils::is_finite(*edge.vertex0()) || !VoronoiUtils::is_finite(*edge.vertex1()))
                return IssueType::FINITE_EDGE_WITH_NON_FINITE_VERTEX;

            if (edge.is_curved() && !edge.cell()->contains_point() && !edge.twin()->cell()->contains_point())
                return IssueType::PARABOLIC_VORONOI_EDGE_WITHOUT_FOCUS_POINT;
        }
    }

    return IssueType::NO_ISSUE_DETECTED;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    VoronoiDiagram::IssueType>::type
VoronoiDiagram::try_to_repair_degenerated_voronoi_diagram(const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    IssueType issue_type = m_issue_type;

    const std::vector<double> fix_angles = {PI / 6, PI / 5, PI / 7, PI / 11};
    for (const double fix_angle : fix_angles) {
        issue_type = try_to_repair_degenerated_voronoi_diagram_by_rotation(segment_begin, segment_end, fix_angle);
        if (issue_type == IssueType::NO_ISSUE_DETECTED) {
            return issue_type;
        }
    }

    return issue_type;
}

inline VD::vertex_type::color_type encode_input_segment_endpoint(const VD::cell_type::source_index_type cell_source_index, const boost::polygon::direction_1d dir)
{
    return (cell_source_index + 1) << 1 | (dir.to_int() ? 1 : 0);
}

template<typename SegmentIterator>
inline typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    typename boost::polygon::segment_point_type<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type
decode_input_segment_endpoint(const VD::vertex_type::color_type color, const SegmentIterator segment_begin, const SegmentIterator segment_end)
{
    using SegmentType = typename std::iterator_traits<SegmentIterator>::value_type;
    using PointType   = typename boost::polygon::segment_traits<SegmentType>::point_type;

    const size_t          segment_idx  = (color >> 1) - 1;
    const SegmentIterator segment_it   = segment_begin + segment_idx;
    const PointType       source_point = boost::polygon::segment_traits<SegmentType>::get(*segment_it, ((color & 1) ? boost::polygon::HIGH :
                                                                                                                      boost::polygon::LOW));
    return source_point;
}

template<typename SegmentIterator>
typename boost::polygon::enable_if<
    typename boost::polygon::gtl_if<typename boost::polygon::is_segment_concept<
        typename boost::polygon::geometry_concept<typename std::iterator_traits<SegmentIterator>::value_type>::type>::type>::type,
    VoronoiDiagram::IssueType>::type
VoronoiDiagram::try_to_repair_degenerated_voronoi_diagram_by_rotation(const SegmentIterator segment_begin,
                                                                      const SegmentIterator segment_end,
                                                                      const double          fix_angle)
{
    using SegmentType = typename std::iterator_traits<SegmentIterator>::value_type;
    using PointType   = typename boost::polygon::segment_traits<SegmentType>::point_type;

    // Copy all segments and rotate their vertices.
    std::vector<VoronoiDiagram::Segment> segments_rotated;
    segments_rotated.reserve(std::distance(segment_begin, segment_end));
    for (auto segment_it = segment_begin; segment_it != segment_end; ++segment_it) {
        PointType from = boost::polygon::segment_traits<SegmentType>::get(*segment_it, boost::polygon::LOW);
        PointType to   = boost::polygon::segment_traits<SegmentType>::get(*segment_it, boost::polygon::HIGH);
        segments_rotated.emplace_back(from.rotated(fix_angle), to.rotated(fix_angle));
    }

    VoronoiDiagram::voronoi_diagram_type voronoi_diagram_rotated;
    boost::polygon::construct_voronoi(segments_rotated.begin(), segments_rotated.end(), &voronoi_diagram_rotated);

    this->copy_to_local(voronoi_diagram_rotated);
    const IssueType issue_type = detect_known_issues(*this, segments_rotated.begin(), segments_rotated.end());

    // We want to remap all Voronoi vertices at the endpoints of input segments
    // to ensure that Voronoi vertices at endpoints will be preserved after rotation.
    // So we assign every Voronoi vertices color to map this Vertex into input segments.
    for (cell_type cell : m_cells) {
        if (cell.is_degenerate())
            continue;

        if (cell.contains_segment()) {
            if (const SegmentCellRange cell_range = VoronoiUtils::compute_segment_cell_range(cell, segments_rotated.begin(), segments_rotated.end()); cell_range.is_valid()) {
                if (cell_range.edge_end->vertex1()->color() == 0) {
                    // Vertex 1 of edge_end points to the starting endpoint of the input segment (from() or line.a).
                    VD::vertex_type::color_type color = encode_input_segment_endpoint(cell.source_index(), boost::polygon::LOW);
                    cell_range.edge_end->vertex1()->color(color);
                }

                if (cell_range.edge_begin->vertex0()->color() == 0) {
                    // Vertex 0 of edge_end points to the ending endpoint of the input segment (to() or line.b).
                    VD::vertex_type::color_type color = encode_input_segment_endpoint(cell.source_index(), boost::polygon::HIGH);
                    cell_range.edge_begin->vertex0()->color(color);
                }
            } else {
                // This could happen when there is a missing Voronoi vertex even after rotation.
                assert(cell_range.is_valid());
            }
        }

        // FIXME @hejllukas: Implement mapping also for source points and not just for source segments.
    }

    // Rotate all Voronoi vertices back.
    // When a Voronoi vertex can be mapped to the input segment endpoint, then we don't need to do rotation back.
    for (vertex_type &vertex : m_vertices) {
        if (vertex.color() == 0) {
            // This vertex isn't mapped to any vertex, so we rotate it back.
            vertex = VoronoiUtils::make_rotated_vertex(vertex, -fix_angle);
        } else {
            // This vertex can be mapped to the input segment endpoint.
            PointType   endpoint = decode_input_segment_endpoint(vertex.color(), segment_begin, segment_end);
            vertex_type endpoint_vertex{double(endpoint.x()), double(endpoint.y())};
            endpoint_vertex.incident_edge(vertex.incident_edge());
            endpoint_vertex.color(vertex.color());
            vertex = endpoint_vertex;
        }
    }

    // We have to clear all marked vertices because some algorithms expect that all vertices have a color equal to 0.
    for (vertex_type &vertex : m_vertices)
        vertex.color(0);

    return issue_type;
}

} // namespace Slic3r::Geometry
