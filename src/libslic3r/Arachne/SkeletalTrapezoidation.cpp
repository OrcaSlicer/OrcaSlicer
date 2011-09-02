//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "SkeletalTrapezoidation.hpp"

#include <boost/log/trivial.hpp>
#include <boost/polygon/polygon.hpp>
#include <queue>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <cassert>
#include <cstdlib>

#include "libslic3r/Geometry/VoronoiUtils.hpp"
#include "ankerl/unordered_dense.h"
#include "libslic3r/Arachne/SkeletalTrapezoidationEdge.hpp"
#include "libslic3r/Arachne/SkeletalTrapezoidationJoint.hpp"
#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"

#ifndef NDEBUG
    #include "libslic3r/EdgeGrid.hpp"
#endif

#define SKELETAL_TRAPEZOIDATION_BEAD_SEARCH_MAX 1000 //A limit to how long it'll keep searching for adjacent beads. Increasing will re-use beadings more often (saving performance), but search longer for beading (costing performance).

namespace Slic3r::Arachne
{

#ifdef ARACHNE_DEBUG
static void export_graph_to_svg(const std::string                                 &path,
                                SkeletalTrapezoidationGraph                       &graph,
                                const Polygons                                    &polys,
                                const std::vector<std::shared_ptr<LineJunctions>> &edge_junctions     = {},
                                const bool                                         beat_count         = true,
                                const bool                                         transition_middles = true,
                                const bool                                         transition_ends    = true)
{
    const std::vector<std::string> colors       = {"blue", "cyan", "red", "orange", "magenta", "pink", "purple", "green", "yellow"};
    coordf_t                       stroke_width = scale_(0.03);
    BoundingBox                    bbox         = get_extents(polys);
    for (const auto &node : graph.nodes)
        bbox.merge(node.p);

    bbox.offset(scale_(1.));

    ::Slic3r::SVG svg(path.c_str(), bbox);
    for (const auto &line : to_lines(polys))
        svg.draw(line, "gray", stroke_width);

    for (const auto &edge : graph.edges)
        svg.draw(Line(edge.from->p, edge.to->p), (edge.data.centralIsSet() && edge.data.isCentral()) ? "blue" : "cyan", stroke_width);

    for (const auto &line_junction : edge_junctions)
        for (const auto &extrusion_junction : *line_junction)
            svg.draw(extrusion_junction.p, "orange", coord_t(stroke_width * 2.));

    if (beat_count) {
        for (const auto &node : graph.nodes) {
            svg.draw(node.p, "red", coord_t(stroke_width * 1.6));
            svg.draw_text(node.p, std::to_string(node.data.bead_count).c_str(), "black", 1);
        }
    }

    if (transition_middles) {
        for (auto &edge : graph.edges) {
            if (std::shared_ptr<std::list<SkeletalTrapezoidationEdge::TransitionMiddle>> transitions = edge.data.getTransitions(); transitions) {
                for (auto &transition : *transitions) {
                    Line edge_line = Line(edge.to->p, edge.from->p);
                    double edge_length = edge_line.length();
                    Point   pt          = edge_line.a + (edge_line.vector().cast<double>() * (double(transition.pos) / edge_length)).cast<coord_t>();
                    svg.draw(pt, "magenta", coord_t(stroke_width * 1.5));
                    svg.draw_text(pt, std::to_string(transition.lower_bead_count).c_str(), "black", 1);
                }
            }
        }
    }

    if (transition_ends) {
        for (auto &edge : graph.edges) {
            if (std::shared_ptr<std::list<SkeletalTrapezoidationEdge::TransitionEnd>> transitions = edge.data.getTransitionEnds(); transitions) {
                for (auto &transition : *transitions) {
                    Line edge_line = Line(edge.to->p, edge.from->p);
                    double edge_length = edge_line.length();
                    Point   pt          = edge_line.a + (edge_line.vector().cast<double>() * (double(transition.pos) / edge_length)).cast<coord_t>();
                    svg.draw(pt, transition.is_lower_end ? "green" : "lime", coord_t(stroke_width * 1.5));
                    svg.draw_text(pt, std::to_string(transition.lower_bead_count).c_str(), "black", 1);
                }
            }
        }
    }
}
#endif

SkeletalTrapezoidation::node_t &SkeletalTrapezoidation::makeNode(const VD::vertex_type &vd_node, Point p) {
    auto he_node_it = vd_node_to_he_node.find(&vd_node);
    if (he_node_it == vd_node_to_he_node.end())
    {
        graph.nodes.emplace_front(SkeletalTrapezoidationJoint(), p);
        node_t& node = graph.nodes.front();
        vd_node_to_he_node.emplace(&vd_node, &node);
        return node;
    }
    else
    {
        return *he_node_it->second;
    }
}

void SkeletalTrapezoidation::transferEdge(const Point &from, const Point &to, const VD::edge_type &vd_edge, edge_t *&prev_edge, const Point &start_source_point, const Point &end_source_point, const std::vector<Segment> &segments) {
    auto he_edge_it = vd_edge_to_he_edge.find(vd_edge.twin());
    if (he_edge_it != vd_edge_to_he_edge.end())
    { // Twin segment(s) have already been made
        edge_t* source_twin = he_edge_it->second;
        assert(source_twin);
        auto end_node_it = vd_node_to_he_node.find(vd_edge.vertex1());
        assert(end_node_it != vd_node_to_he_node.end());
        node_t* end_node = end_node_it->second;
        for (edge_t* twin = source_twin; ;twin = twin->prev->twin->prev)
        {
            if(!twin)
            {
                BOOST_LOG_TRIVIAL(warning) << "Encountered a voronoi edge without twin.";
                continue; //Prevent reading unallocated memory.
            }
            assert(twin);
            graph.edges.emplace_front(SkeletalTrapezoidationEdge());
            edge_t* edge = &graph.edges.front();
            edge->from = twin->to;
            edge->to = twin->from;
            edge->twin = twin;
            twin->twin = edge;
            edge->from->incident_edge = edge;
            
            if (prev_edge)
            {
                edge->prev = prev_edge;
                prev_edge->next = edge;
            }

            prev_edge = edge;

            if (prev_edge->to == end_node)
            {
                return;
            }
            
            if (!twin->prev || !twin->prev->twin || !twin->prev->twin->prev)
            {
                BOOST_LOG_TRIVIAL(error) << "Discretized segment behaves oddly!";
                return;
            }
            
            assert(twin->prev); // Forth rib
            assert(twin->prev->twin); // Back rib
            assert(twin->prev->twin->prev); // Prev segment along parabola
            
            graph.makeRib(prev_edge, start_source_point, end_source_point);
        }
        assert(prev_edge);
    }
    else
    {
        Points discretized = discretize(vd_edge, segments);
        assert(discretized.size() >= 2);
        if(discretized.size() < 2)
        {
            BOOST_LOG_TRIVIAL(warning) << "Discretized Voronoi edge is degenerate.";
        }
        
        assert(!prev_edge || prev_edge->to);
        if(prev_edge && !prev_edge->to)
        {
            BOOST_LOG_TRIVIAL(warning) << "Previous edge doesn't go anywhere.";
        }
        node_t* v0 = (prev_edge)? prev_edge->to : &makeNode(*vd_edge.vertex0(), from); // TODO: investigate whether boost:voronoi can produce multiple verts and violates consistency
        Point p0 = discretized.front();
        for (size_t p1_idx = 1; p1_idx < discretized.size(); p1_idx++)
        {
            Point p1 = discretized[p1_idx];
            node_t* v1;
            if (p1_idx < discretized.size() - 1)
            {
                graph.nodes.emplace_front(SkeletalTrapezoidationJoint(), p1);
                v1 = &graph.nodes.front();
            }
            else
            {
                v1 = &makeNode(*vd_edge.vertex1(), to);
            }

            graph.edges.emplace_front(SkeletalTrapezoidationEdge());
            edge_t* edge = &graph.edges.front();
            edge->from = v0;
            edge->to = v1;
            edge->from->incident_edge = edge;
            
            if (prev_edge)
            {
                edge->prev = prev_edge;
                prev_edge->next = edge;
            }
            
            prev_edge = edge;
            p0 = p1;
            v0 = v1;
            
            if (p1_idx < discretized.size() - 1) { // Rib for last segment gets introduced outside this function!
                graph.makeRib(prev_edge, start_source_point, end_source_point);
            }
        }
        assert(prev_edge);
        vd_edge_to_he_edge.emplace(&vd_edge, prev_edge);
    }
}

Points SkeletalTrapezoidation::discretize(const VD::edge_type& vd_edge, const std::vector<Segment>& segments)
{
    assert(Geometry::VoronoiUtils::is_in_range<coord_t>(vd_edge));

    /*Terminology in this function assumes that the edge moves horizontally from
    left to right. This is not necessarily the case; the edge can go in any
    direction, but it helps to picture it in a certain direction in your head.*/

    const VD::cell_type *left_cell  = vd_edge.cell();
    const VD::cell_type *right_cell = vd_edge.twin()->cell();

    Point start = Geometry::VoronoiUtils::to_point(vd_edge.vertex0()).cast<coord_t>();
    Point end   = Geometry::VoronoiUtils::to_point(vd_edge.vertex1()).cast<coord_t>();

    bool point_left = left_cell->contains_point();
    bool point_right = right_cell->contains_point();
    if ((!point_left && !point_right) || vd_edge.is_secondary()) // Source vert is directly connected to source segment
    {
        return Points({ start, end });
    }
    else if (point_left != point_right) //This is a parabolic edge between a point and a line.
    {
        Point          p = Geometry::VoronoiUtils::get_source_point(*(point_left ? left_cell : right_cell), segments.begin(), segments.end());
        const Segment& s = Geometry::VoronoiUtils::get_source_segment(*(point_left ? right_cell : left_cell), segments.begin(), segments.end());
        return Geometry::VoronoiUtils::discretize_parabola(p, s, start, end, discretization_step_size, transitioning_angle);
    }
    else //This is a straight edge between two points.
    {
        /*While the edge is straight, it is still discretized since the part
        becomes narrower between the two points. As such it may need different
        beadings along the way.*/
        Point   left_point    = Geometry::VoronoiUtils::get_source_point(*left_cell, segments.begin(), segments.end());
        Point   right_point   = Geometry::VoronoiUtils::get_source_point(*right_cell, segments.begin(), segments.end());
        coord_t d             = (right_point - left_point).cast<int64_t>().norm();
        Point   middle        = (left_point + right_point) / 2;
        Point   x_axis_dir    = perp(Point(right_point - left_point));
        coord_t x_axis_length = x_axis_dir.cast<int64_t>().norm();

        const auto projected_x = [x_axis_dir, x_axis_length, middle](Point from) //Project a point on the edge.
        {
            Point vec = from - middle;
            assert(( vec.cast<int64_t>().dot(x_axis_dir.cast<int64_t>())/ int64_t(x_axis_length)) <= std::numeric_limits<coord_t>::max());
            coord_t x = vec.cast<int64_t>().dot(x_axis_dir.cast<int64_t>()) / int64_t(x_axis_length);
            return x;
        };
        
        coord_t start_x = projected_x(start);
        coord_t end_x = projected_x(end);

        //Part of the edge will be bound to the markings on the endpoints of the edge. Calculate how far that is.
        float bound = 0.5 / tan((M_PI - transitioning_angle) * 0.5);
        int64_t marking_start_x = - int64_t(d) * bound;
        int64_t marking_end_x = int64_t(d) * bound;

        assert((middle.cast<int64_t>() + x_axis_dir.cast<int64_t>() * marking_start_x / int64_t(x_axis_length)).x() <= std::numeric_limits<coord_t>::max());
        assert((middle.cast<int64_t>() + x_axis_dir.cast<int64_t>() * marking_start_x / int64_t(x_axis_length)).y() <= std::numeric_limits<coord_t>::max());
        assert((middle.cast<int64_t>() + x_axis_dir.cast<int64_t>() * marking_end_x / int64_t(x_axis_length)).x() <= std::numeric_limits<coord_t>::max());
        assert((middle.cast<int64_t>() + x_axis_dir.cast<int64_t>() * marking_end_x / int64_t(x_axis_length)).y() <= std::numeric_limits<coord_t>::max());
        Point marking_start = middle + (x_axis_dir.cast<int64_t>() * marking_start_x / int64_t(x_axis_length)).cast<coord_t>();
        Point marking_end = middle + (x_axis_dir.cast<int64_t>() * marking_end_x / int64_t(x_axis_length)).cast<coord_t>();
        int64_t direction = 1;
        
        if (start_x > end_x) //Oops, the Voronoi edge is the other way around.
        {
            direction = -1;
            std::swap(marking_start, marking_end);
            std::swap(marking_start_x, marking_end_x);
        }

        //Start generating points along the edge.
        Point a = start;
        Point b = end;
        Points ret;
        ret.emplace_back(a);

        //Introduce an extra edge at the borders of the markings?
        bool add_marking_start = marking_start_x * direction > int64_t(start_x) * direction;
        bool add_marking_end = marking_end_x * direction > int64_t(start_x) * direction;

        //The edge's length may not be divisible by the step size, so calculate an integer step count and evenly distribute the vertices among those.
        Point ab = b - a;
        coord_t ab_size = ab.cast<int64_t>().norm();
        coord_t step_count = (ab_size + discretization_step_size / 2) / discretization_step_size;
        if (step_count % 2 == 1)
        {
            step_count++; // enforce a discretization point being added in the middle
        }
        for (coord_t step = 1; step < step_count; step++)
        {
            Point here = a + (ab.cast<int64_t>() * int64_t(step) / int64_t(step_count)).cast<coord_t>(); //Now simply interpolate the coordinates to get the new vertices!
            coord_t x_here = projected_x(here); //If we've surpassed the position of the extra markings, we may need to insert them first.
            if (add_marking_start && marking_start_x * direction < int64_t(x_here) * direction)
            {
                ret.emplace_back(marking_start);
                add_marking_start = false;
            }
            if (add_marking_end && marking_end_x * direction < int64_t(x_here) * direction)
            {
                ret.emplace_back(marking_end);
                add_marking_end = false;
            }
            ret.emplace_back(here);
        }
        if (add_marking_end && marking_end_x * direction < int64_t(end_x) * direction)
        {
            ret.emplace_back(marking_end);
        }
        ret.emplace_back(b);
        return ret;
    }
}

SkeletalTrapezoidation::SkeletalTrapezoidation(const Polygons& polys, const BeadingStrategy& beading_strategy,
                                               double transitioning_angle, coord_t discretization_step_size,
                                               coord_t transition_filter_dist, coord_t allowed_filter_deviation,
                                               coord_t beading_propagation_transition_dist
    ): transitioning_angle(transitioning_angle),
    discretization_step_size(discretization_step_size),
    transition_filter_dist(transition_filter_dist),
    allowed_filter_deviation(allowed_filter_deviation),
    beading_propagation_transition_dist(beading_propagation_transition_dist),
    beading_strategy(beading_strategy)
{
    constructFromPolygons(polys);
}

void SkeletalTrapezoidation::constructFromPolygons(const Polygons& polys)
{
#ifdef ARACHNE_DEBUG
    this->outline = polys;
#endif

    // Check self intersections.
    assert([&polys]() -> bool {
        EdgeGrid::Grid grid;
        grid.set_bbox(get_extents(polys));
        grid.create(polys, scaled<coord_t>(10.));
        return !grid.has_intersecting_edges();
    }());

    vd_edge_to_he_edge.clear();
    vd_node_to_he_node.clear();

    std::vector<Segment> segments;
    for (size_t poly_idx = 0; poly_idx < polys.size(); poly_idx++)
        for (size_t point_idx = 0; point_idx < polys[poly_idx].size(); point_idx++)
            segments.emplace_back(&polys, poly_idx, point_idx);

#ifdef ARACHNE_DEBUG
    {
        static int iRun = 0;
        BoundingBox bbox = get_extents(polys);
        SVG svg(debug_out_path("arachne_voronoi-input-%d.svg", iRun++).c_str(), bbox);
        svg.draw_outline(polys, "black", scaled<coordf_t>(0.03f));
    }
#endif

    VD voronoi_diagram;
    voronoi_diagram.construct_voronoi(segments.cbegin(), segments.cend());

#ifdef ARACHNE_DEBUG_VORONOI
    {
        static int iRun = 0;
        dump_voronoi_to_svg(debug_out_path("arachne_voronoi-diagram-%d.svg", iRun++).c_str(), voronoi_diagram, to_points(polys), to_lines(polys));
    }
#endif

    assert(this->graph.edges.empty() && this->graph.nodes.empty() && this->vd_edge_to_he_edge.empty() && this->vd_node_to_he_node.empty());
    for (const VD::cell_type &cell : voronoi_diagram.cells()) {
        if (!cell.incident_edge())
            continue; // There is no spoon

        Point                start_source_point;
        Point                end_source_point;
        const VD::edge_type *starting_voronoi_edge = nullptr;
        const VD::edge_type *ending_voronoi_edge   = nullptr;
        // Compute and store result in above variables

        if (cell.contains_point()) {
            Geometry::PointCellRange<Point> cell_range = Geometry::VoronoiUtils::compute_point_cell_range(cell, segments.cbegin(), segments.cend());
            start_source_point    = cell_range.source_point;
            end_source_point      = cell_range.source_point;
            starting_voronoi_edge = cell_range.edge_begin;
            ending_voronoi_edge   = cell_range.edge_end;

            if (!cell_range.is_valid())
                continue;
        } else {
            assert(cell.contains_segment());
            Geometry::SegmentCellRange<Point> cell_range = Geometry::VoronoiUtils::compute_segment_cell_range(cell, segments.cbegin(), segments.cend());
            assert(cell_range.is_valid());
            start_source_point    = cell_range.source_segment_start_point;
            end_source_point      = cell_range.source_segment_end_point;
            starting_voronoi_edge = cell_range.edge_begin;
            ending_voronoi_edge   = cell_range.edge_end;
        }

        if (!starting_voronoi_edge || !ending_voronoi_edge) {
            assert(false && "Each cell should start / end in a polygon vertex");
            continue;
        }

        // Copy start to end edge to graph
        assert(Geometry::VoronoiUtils::is_in_range<coord_t>(*starting_voronoi_edge));
        edge_t *prev_edge = nullptr;
        transferEdge(start_source_point, Geometry::VoronoiUtils::to_point(starting_voronoi_edge->vertex1()).cast<coord_t>(), *starting_voronoi_edge, prev_edge, start_source_point, end_source_point, segments);
        node_t *starting_node                    = vd_node_to_he_node[starting_voronoi_edge->vertex0()];
        starting_node->data.distance_to_boundary = 0;

        graph.makeRib(prev_edge, start_source_point, end_source_point);
        for (const VD::edge_type* vd_edge = starting_voronoi_edge->next(); vd_edge != ending_voronoi_edge; vd_edge = vd_edge->next()) {
            assert(vd_edge->is_finite());
            assert(Geometry::VoronoiUtils::is_in_range<coord_t>(*vd_edge));

            Point v1 = Geometry::VoronoiUtils::to_point(vd_edge->vertex0()).cast<coord_t>();
            Point v2 = Geometry::VoronoiUtils::to_point(vd_edge->vertex1()).cast<coord_t>();
            transferEdge(v1, v2, *vd_edge, prev_edge, start_source_point, end_source_point, segments);
            graph.makeRib(prev_edge, start_source_point, end_source_point);
        }

        transferEdge(Geometry::VoronoiUtils::to_point(ending_voronoi_edge->vertex0()).cast<coord_t>(), end_source_point, *ending_voronoi_edge, prev_edge, start_source_point, end_source_point, segments);
        prev_edge->to->data.distance_to_boundary = 0;
    }

#ifdef ARACHNE_DEBUG
    assert(Geometry::VoronoiUtilsCgal::is_voronoi_diagram_planar_intersection(voronoi_diagram));
#endif

    separatePointyQuadEndNodes();

    graph.collapseSmallEdges();

    // Set [incident_edge] the first possible edge that way we can iterate over all reachable edges from node.incident_edge,
    // without needing to iterate backward
    for (edge_t& edge : graph.edges)
        if (!edge.prev)
            edge.from->incident_edge = &edge;
}

using NodeSet = SkeletalTrapezoidation::NodeSet;

void SkeletalTrapezoidation::separatePointyQuadEndNodes()
{
    NodeSet visited_nodes;
    for (edge_t& edge : graph.edges)
    {
        if (edge.prev) 
        {
            continue;
        }
        edge_t* quad_start = &edge;
        if (visited_nodes.find(quad_start->from) == visited_nodes.end())
        {
            visited_nodes.emplace(quad_start->from);
        }
        else
        { // Needs to be duplicated
            graph.nodes.emplace_back(*quad_start->from);
            node_t* new_node = &graph.nodes.back();
            new_node->incident_edge = quad_start;
            quad_start->from = new_node;
            quad_start->twin->to = new_node;
        }
    }
}

//
// ^^^^^^^^^^^^^^^^^^^^^
//    INITIALIZATION
// =====================
//
// =====================
//    TRANSTISIONING
// vvvvvvvvvvvvvvvvvvvvv
//

void SkeletalTrapezoidation::generateToolpaths(std::vector<VariableWidthLines> &generated_toolpaths, bool filter_outermost_central_edges)
{
#ifdef ARACHNE_DEBUG
    static int iRun = 0;
#endif

    p_generated_toolpaths = &generated_toolpaths;

    updateIsCentral();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-updateIsCentral-final-%d.svg", iRun), this->graph, this->outline);
#endif

    filterCentral(central_filter_dist());

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-filterCentral-final-%d.svg", iRun), this->graph, this->outline);
#endif

    if (filter_outermost_central_edges)
        filterOuterCentral();

    updateBeadCount();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-updateBeadCount-final-%d.svg", iRun), this->graph, this->outline);
#endif

    filterNoncentralRegions();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-filterNoncentralRegions-final-%d.svg", iRun), this->graph, this->outline);
#endif

    generateTransitioningRibs();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateTransitioningRibs-final-%d.svg", iRun), this->graph, this->outline);
#endif

    generateExtraRibs();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateExtraRibs-final-%d.svg", iRun), this->graph, this->outline);
#endif

    generateSegments();

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateSegments-final-%d.svg", iRun), this->graph, this->outline);
#endif

#ifdef ARACHNE_DEBUG
    ++iRun;
#endif
}

void SkeletalTrapezoidation::updateIsCentral()
{
    //                                            _.-'^`      A and B are the endpoints of an edge we're checking.
    //                                      _.-'^`            Part of the line AB will be used as a cap,
    //                                _.-'^` \                because the polygon is too narrow there.
    //                          _.-'^`        \               If |AB| minus the cap is still bigger than dR,
    //                    _.-'^`               \ R2           the edge AB is considered central. It's then
    //              _.-'^` \              _.-'\`\             significant compared to the edges around it.
    //        _.-'^`        \R1     _.-'^`     '`\ dR
    //  _.-'^`a/2            \_.-'^`a             \           Line AR2 is parallel to the polygon contour.
    //  `^'-._````````````````A```````````v````````B```````   dR is the remaining diameter at B.
    //        `^'-._                     dD = |AB|            As a result, AB is less often central if the polygon
    //              `^'-._                                    corner is obtuse.
    //                             sin a = dR / dD

    coord_t outer_edge_filter_length = beading_strategy.getTransitionThickness(0) / 2;

    float cap = sin(beading_strategy.getTransitioningAngle() * 0.5); // = cos(bisector_angle / 2)
    for (edge_t& edge: graph.edges)
    {
        assert(edge.twin);
        if(!edge.twin)
        {
            BOOST_LOG_TRIVIAL(warning) << "Encountered a Voronoi edge without twin!";
            continue;
        }
        if(edge.twin->data.centralIsSet())
        {
            edge.data.setIsCentral(edge.twin->data.isCentral());
        }
        else if(edge.data.type == SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD)
        {
            edge.data.setIsCentral(false);
        }
        else if(std::max(edge.from->data.distance_to_boundary, edge.to->data.distance_to_boundary) < outer_edge_filter_length)
        {
            edge.data.setIsCentral(false);
        }
        else
        {
            Point a = edge.from->p;
            Point b = edge.to->p;
            Point ab = b - a;
            coord_t dR = std::abs(edge.to->data.distance_to_boundary - edge.from->data.distance_to_boundary);
            coord_t dD = ab.cast<int64_t>().norm();
            edge.data.setIsCentral(dR < dD * cap);
        }
    }
}

void SkeletalTrapezoidation::filterCentral(coord_t max_length)
{
    for (edge_t& edge : graph.edges)
    {
        if (isEndOfCentral(edge) && edge.to->isLocalMaximum() && !edge.to->isLocalMaximum())
        {
            filterCentral(edge.twin, 0, max_length);
        }
    }
}

bool SkeletalTrapezoidation::filterCentral(edge_t* starting_edge, coord_t traveled_dist, coord_t max_length)
{
    coord_t length = (starting_edge->from->p - starting_edge->to->p).cast<int64_t>().norm();
    if (traveled_dist + length > max_length)
    {
        return false;
    }
    
    bool should_dissolve = true; //Should we unmark this as central and propagate that?
    for (edge_t* next_edge = starting_edge->next; next_edge && next_edge != starting_edge->twin; next_edge = next_edge->twin->next)
    {
        if (next_edge->data.isCentral())
        {
            should_dissolve &= filterCentral(next_edge, traveled_dist + length, max_length);
        }
    }

    should_dissolve &= !starting_edge->to->isLocalMaximum(); // Don't filter central regions with a local maximum!
    if (should_dissolve)
    {
        starting_edge->data.setIsCentral(false);
        starting_edge->twin->data.setIsCentral(false);
    }
    return should_dissolve;
}

void SkeletalTrapezoidation::filterOuterCentral()
{
    for (edge_t& edge : graph.edges)
    {
        if (!edge.prev)
        {
            edge.data.setIsCentral(false);
            edge.twin->data.setIsCentral(false);
        }
    }
}

void SkeletalTrapezoidation::updateBeadCount()
{
    for (edge_t& edge : graph.edges)
    {
        if (edge.data.isCentral())
        {
            edge.to->data.bead_count = beading_strategy.getOptimalBeadCount(edge.to->data.distance_to_boundary * 2);
        }
    }

    // Fix bead count at locally maximal R, also for central regions!! See TODO s in generateTransitionEnd(.)
    for (node_t& node : graph.nodes)
    {
        if (node.isLocalMaximum())
        {
            if (node.data.distance_to_boundary < 0)
            {
                BOOST_LOG_TRIVIAL(warning) << "Distance to boundary not yet computed for local maximum!";
                node.data.distance_to_boundary = std::numeric_limits<coord_t>::max();
                edge_t* edge = node.incident_edge;
                do
                {
                    node.data.distance_to_boundary = std::min(node.data.distance_to_boundary, edge->to->data.distance_to_boundary + coord_t((edge->from->p - edge->to->p).cast<int64_t>().norm()));
                } while (edge = edge->twin->next, edge != node.incident_edge);
            }
            coord_t bead_count = beading_strategy.getOptimalBeadCount(node.data.distance_to_boundary * 2);
            node.data.bead_count = bead_count;
        }
    }
}

void SkeletalTrapezoidation::filterNoncentralRegions()
{
    for (edge_t& edge : graph.edges)
    {
        if (!isEndOfCentral(edge))
        {
            continue;
        }
        if(edge.to->data.bead_count < 0 && edge.to->data.distance_to_boundary != 0)
        {
            BOOST_LOG_TRIVIAL(warning) << "Encountered an uninitialized bead at the boundary!";
        }
        assert(edge.to->data.bead_count >= 0 || edge.to->data.distance_to_boundary == 0);
        const coord_t max_dist = scaled<coord_t>(0.4);
        filterNoncentralRegions(&edge, edge.to->data.bead_count, 0, max_dist);
    }
}

bool SkeletalTrapezoidation::filterNoncentralRegions(edge_t* to_edge, coord_t bead_count, coord_t traveled_dist, coord_t max_dist)
{
    coord_t r = to_edge->to->data.distance_to_boundary;

    edge_t* next_edge = to_edge->next;
    for (; next_edge && next_edge != to_edge->twin; next_edge = next_edge->twin->next)
    {
        if (next_edge->to->data.distance_to_boundary >= r || shorter_then(next_edge->to->p - next_edge->from->p, scaled<coord_t>(0.01)))
        {
            break; // Only walk upward
        }
    }
    if (next_edge == to_edge->twin || ! next_edge)
    {
        return false;
    }

    const coord_t length = (next_edge->to->p - next_edge->from->p).cast<int64_t>().norm();

    bool dissolve = false;
    if (next_edge->to->data.bead_count == bead_count)
    {
        dissolve = true;
    }
    else if (next_edge->to->data.bead_count < 0)
    {
        dissolve = filterNoncentralRegions(next_edge, bead_count, traveled_dist + length, max_dist);
    }
    else // Upward bead count is different
    {
        // Dissolve if two central regions with different bead count are closer together than the max_dist (= transition distance)
        dissolve = (traveled_dist + length < max_dist) && std::abs(next_edge->to->data.bead_count - bead_count) == 1;
    }

    if (dissolve)
    {
        next_edge->data.setIsCentral(true);
        next_edge->twin->data.setIsCentral(true);
        next_edge->to->data.bead_count = beading_strategy.getOptimalBeadCount(next_edge->to->data.distance_to_boundary * 2);
        next_edge->to->data.transition_ratio = 0;
    }
    return dissolve; // Dissolving only depend on the one edge going upward. There cannot be multiple edges going upward.
}

void SkeletalTrapezoidation::generateTransitioningRibs()
{
    // Store the upward edges to the transitions.
    // We only store the halfedge for which the distance_to_boundary is higher at the end than at the beginning.
    ptr_vector_t<std::list<TransitionMiddle>> edge_transitions;
    generateTransitionMids(edge_transitions);

    for (edge_t& edge : graph.edges)
    { // Check if there is a transition in between nodes with different bead counts
        if (edge.data.isCentral() && edge.from->data.bead_count != edge.to->data.bead_count)
        {
            assert(edge.data.hasTransitions() || edge.twin->data.hasTransitions());
        }
    }
 
    filterTransitionMids();

#ifdef ARACHNE_DEBUG
    static int iRun = 0;
    export_graph_to_svg(debug_out_path("ST-generateTransitioningRibs-mids-%d.svg", iRun++), this->graph, this->outline);
#endif

    ptr_vector_t<std::list<TransitionEnd>> edge_transition_ends; // We only map the half edge in the upward direction. mapped items are not sorted
    generateAllTransitionEnds(edge_transition_ends);

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateTransitioningRibs-ends-%d.svg", iRun++), this->graph, this->outline);
#endif

    applyTransitions(edge_transition_ends);
    // Note that the shared pointer lists will be out of scope and thus destroyed here, since the remaining refs are weak_ptr.

#ifdef ARACHNE_DEBUG
    ++iRun;
#endif
}


void SkeletalTrapezoidation::generateTransitionMids(ptr_vector_t<std::list<TransitionMiddle>>& edge_transitions)
{
    for (edge_t& edge : graph.edges)
    {
        assert(edge.data.centralIsSet());
        if (!edge.data.isCentral())
        { // Only central regions introduce transitions
            continue;
        }
        coord_t start_R = edge.from->data.distance_to_boundary;
        coord_t end_R = edge.to->data.distance_to_boundary;
        int start_bead_count = edge.from->data.bead_count;
        int end_bead_count = edge.to->data.bead_count;

        if (start_R == end_R)
        { // No transitions occur when both end points have the same distance_to_boundary
            assert(edge.from->data.bead_count == edge.to->data.bead_count);
            if(edge.from->data.bead_count != edge.to->data.bead_count)
            {
                BOOST_LOG_TRIVIAL(warning) << "Bead count " << edge.from->data.bead_count << " is different from " << edge.to->data.bead_count << " even though distance to boundary is the same.";
            }
            continue;
        }
        else if (start_R > end_R)
        { // Only consider those half-edges which are going from a lower to a higher distance_to_boundary
            continue;
        }

        if (edge.from->data.bead_count == edge.to->data.bead_count)
        { // No transitions should occur according to the enforced bead counts
            continue;
        }

        if (start_bead_count > beading_strategy.getOptimalBeadCount(start_R * 2)
            || end_bead_count > beading_strategy.getOptimalBeadCount(end_R * 2))
        { // Wasn't the case earlier in this function because of already introduced transitions
            BOOST_LOG_TRIVIAL(error) << "transitioning segment overlap! (?)";
        }
        assert(start_R < end_R);
        if(start_R >= end_R)
        {
            BOOST_LOG_TRIVIAL(warning) << "Transitioning the wrong way around! This function expects to transition from small R to big R, but was transitioning from " << start_R << " to " << end_R;
        }
        coord_t edge_size = (edge.from->p - edge.to->p).cast<int64_t>().norm();
        for (int transition_lower_bead_count = start_bead_count; transition_lower_bead_count < end_bead_count; transition_lower_bead_count++)
        {
            coord_t mid_R = beading_strategy.getTransitionThickness(transition_lower_bead_count) / 2;
            if (mid_R > end_R)
            {
                BOOST_LOG_TRIVIAL(error) << "transition on segment lies outside of segment!";
                mid_R = end_R;
            }
            if (mid_R < start_R)
            {
                BOOST_LOG_TRIVIAL(error) << "transition on segment lies outside of segment!";
                mid_R = start_R;
            }
            coord_t mid_pos = int64_t(edge_size) * int64_t(mid_R - start_R) / int64_t(end_R - start_R);

            assert(mid_pos >= 0);
            assert(mid_pos <= edge_size);
            if(mid_pos < 0 || mid_pos > edge_size)
            {
                BOOST_LOG_TRIVIAL(warning) << "Transition mid is out of bounds of the edge.";
            }
            auto transitions = edge.data.getTransitions();
            constexpr bool ignore_empty = true;
            assert((! edge.data.hasTransitions(ignore_empty)) || mid_pos >= transitions->back().pos);
            if (! edge.data.hasTransitions(ignore_empty))
            {
                edge_transitions.emplace_back(std::make_shared<std::list<TransitionMiddle>>());
                edge.data.setTransitions(edge_transitions.back());  // initialization
                transitions = edge.data.getTransitions();
            }
            transitions->emplace_back(mid_pos, transition_lower_bead_count, mid_R);
        }
        assert((edge.from->data.bead_count == edge.to->data.bead_count) || edge.data.hasTransitions());
    }
}

void SkeletalTrapezoidation::filterTransitionMids()
{
    for (edge_t& edge : graph.edges)
    {
        if (! edge.data.hasTransitions())
        {
            continue;
        }
        auto& transitions = *edge.data.getTransitions();

        // This is how stuff should be stored in transitions
        assert(transitions.front().lower_bead_count <= transitions.back().lower_bead_count);
        assert(edge.from->data.distance_to_boundary <= edge.to->data.distance_to_boundary);
        
        const Point a = edge.from->p;
        const Point b = edge.to->p;
        Point ab = b - a;
        coord_t ab_size = ab.cast<int64_t>().norm();

        bool going_up = true;
        std::list<TransitionMidRef> to_be_dissolved_back = dissolveNearbyTransitions(&edge, transitions.back(), ab_size - transitions.back().pos, transition_filter_dist, going_up);
        bool should_dissolve_back = !to_be_dissolved_back.empty();
        for (TransitionMidRef& ref : to_be_dissolved_back)
        {
            dissolveBeadCountRegion(&edge, transitions.back().lower_bead_count + 1, transitions.back().lower_bead_count);
            ref.edge->data.getTransitions()->erase(ref.transition_it);
        }

        {
            coord_t trans_bead_count = transitions.back().lower_bead_count;
            coord_t upper_transition_half_length = (1.0 - beading_strategy.getTransitionAnchorPos(trans_bead_count)) * beading_strategy.getTransitioningLength(trans_bead_count);
            should_dissolve_back |= filterEndOfCentralTransition(&edge, ab_size - transitions.back().pos, upper_transition_half_length, trans_bead_count);
        }
        
        if (should_dissolve_back)
        {
            transitions.pop_back();
        }
        if (transitions.empty())
        { // FilterEndOfCentralTransition gives inconsistent new bead count when executing for the same transition in two directions.
            continue;
        }

        going_up = false;
        std::list<TransitionMidRef> to_be_dissolved_front = dissolveNearbyTransitions(edge.twin, transitions.front(), transitions.front().pos, transition_filter_dist, going_up);
        bool should_dissolve_front = !to_be_dissolved_front.empty();
        for (TransitionMidRef& ref : to_be_dissolved_front)
        {
            dissolveBeadCountRegion(edge.twin, transitions.front().lower_bead_count, transitions.front().lower_bead_count + 1);
            ref.edge->data.getTransitions()->erase(ref.transition_it);
        }

        {
            coord_t trans_bead_count = transitions.front().lower_bead_count;
            coord_t lower_transition_half_length = beading_strategy.getTransitionAnchorPos(trans_bead_count) * beading_strategy.getTransitioningLength(trans_bead_count);
            should_dissolve_front |= filterEndOfCentralTransition(edge.twin, transitions.front().pos, lower_transition_half_length, trans_bead_count + 1);
        }
        
        if (should_dissolve_front)
        {
            transitions.pop_front();
        }
        if (transitions.empty())
        { // FilterEndOfCentralTransition gives inconsistent new bead count when executing for the same transition in two directions.
            continue;
        }
    }
}

std::list<SkeletalTrapezoidation::TransitionMidRef> SkeletalTrapezoidation::dissolveNearbyTransitions(edge_t* edge_to_start, TransitionMiddle& origin_transition, coord_t traveled_dist, coord_t max_dist, bool going_up)
{
    std::list<TransitionMidRef> to_be_dissolved;
    if (traveled_dist > max_dist)
        return to_be_dissolved;

    bool should_dissolve = true;
    for (edge_t* edge = edge_to_start->next; edge && edge != edge_to_start->twin; edge = edge->twin->next){
        if (!edge->data.isCentral())
            continue;

        Point a = edge->from->p;
        Point b = edge->to->p;
        Point ab = b - a;
        coord_t ab_size = ab.cast<int64_t>().norm();
        bool is_aligned = edge->isUpward();
        edge_t* aligned_edge = is_aligned? edge : edge->twin;
        bool seen_transition_on_this_edge = false;

        const coord_t origin_radius          = origin_transition.feature_radius;
        const coord_t radius_here            = edge->from->data.distance_to_boundary;
        const bool    dissolve_result_is_odd = bool(origin_transition.lower_bead_count % 2) == going_up;
        const coord_t width_deviation        = std::abs(origin_radius - radius_here) * 2; // times by two because the deviation happens at both sides of the significant edge
        const coord_t line_width_deviation = dissolve_result_is_odd ? width_deviation : width_deviation / 2; // assume the deviation will be split over either 1 or 2 lines, i.e. assume wall_distribution_count = 1
        if (line_width_deviation > allowed_filter_deviation)
            should_dissolve = false;

        if (should_dissolve && aligned_edge->data.hasTransitions()) {
            auto& transitions = *aligned_edge->data.getTransitions();
            for (auto transition_it = transitions.begin(); transition_it != transitions.end(); ++ transition_it) { // Note: this is not necessarily iterating in the traveling direction!
                // Check whether we should dissolve
                coord_t pos = is_aligned? transition_it->pos : ab_size - transition_it->pos;
                if (traveled_dist + pos < max_dist && transition_it->lower_bead_count == origin_transition.lower_bead_count) { // Only dissolve local optima
                    if (traveled_dist + pos < beading_strategy.getTransitioningLength(transition_it->lower_bead_count)) {
                        // Consecutive transitions both in/decreasing in bead count should never be closer together than the transition distance
                        assert(going_up != is_aligned || transition_it->lower_bead_count == 0); 
                    }
                    to_be_dissolved.emplace_back(aligned_edge, transition_it);
                    seen_transition_on_this_edge = true;
                }
            }
        }
        if (should_dissolve && !seen_transition_on_this_edge) {
            std::list<SkeletalTrapezoidation::TransitionMidRef> to_be_dissolved_here = dissolveNearbyTransitions(edge, origin_transition, traveled_dist + ab_size, max_dist, going_up);
            if (to_be_dissolved_here.empty()) { // The region is too long to be dissolved in this direction, so it cannot be dissolved in any direction.
                to_be_dissolved.clear();
                return to_be_dissolved;
            }
            to_be_dissolved.splice(to_be_dissolved.end(), to_be_dissolved_here); // Transfer to_be_dissolved_here into to_be_dissolved
            should_dissolve = should_dissolve && !to_be_dissolved.empty();
        }
    }

    if (!should_dissolve)
        to_be_dissolved.clear();

    return to_be_dissolved;
}


void SkeletalTrapezoidation::dissolveBeadCountRegion(edge_t* edge_to_start, coord_t from_bead_count, coord_t to_bead_count)
{
    assert(from_bead_count != to_bead_count);
    if (edge_to_start->to->data.bead_count != from_bead_count)
        return;

    edge_to_start->to->data.bead_count = to_bead_count;
    for (edge_t* edge = edge_to_start->next; edge && edge != edge_to_start->twin; edge = edge->twin->next)
    {
        if (!edge->data.isCentral())
        {
            continue;
        }
        dissolveBeadCountRegion(edge, from_bead_count, to_bead_count);
    }
}

bool SkeletalTrapezoidation::filterEndOfCentralTransition(edge_t* edge_to_start, coord_t traveled_dist, coord_t max_dist, coord_t replacing_bead_count)
{
    if (traveled_dist > max_dist)
    {
        return false;
    }
    
    bool is_end_of_central = true;
    bool should_dissolve = false;
    for (edge_t* next_edge = edge_to_start->next; next_edge && next_edge != edge_to_start->twin; next_edge = next_edge->twin->next)
    {
        if (next_edge->data.isCentral())
        {
            coord_t length = (next_edge->to->p - next_edge->from->p).cast<int64_t>().norm();
            should_dissolve |= filterEndOfCentralTransition(next_edge, traveled_dist + length, max_dist, replacing_bead_count);
            is_end_of_central = false;
        }
    }
    if (is_end_of_central && traveled_dist < max_dist)
    {
        should_dissolve = true;
    }
    
    if (should_dissolve)
    {
        edge_to_start->to->data.bead_count = replacing_bead_count;
    }
    return should_dissolve;
}

void SkeletalTrapezoidation::generateAllTransitionEnds(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    for (edge_t& edge : graph.edges)
    {
        if (! edge.data.hasTransitions())
        {
            continue;
        }
        auto& transition_positions = *edge.data.getTransitions();

        assert(edge.from->data.distance_to_boundary <= edge.to->data.distance_to_boundary);
        for (TransitionMiddle& transition_middle : transition_positions)
        {
            assert(transition_positions.front().pos <= transition_middle.pos);
            assert(transition_middle.pos <= transition_positions.back().pos);
            generateTransitionEnds(edge, transition_middle.pos, transition_middle.lower_bead_count, edge_transition_ends);
        }
    }
}

void SkeletalTrapezoidation::generateTransitionEnds(edge_t& edge, coord_t mid_pos, coord_t lower_bead_count, ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    const Point a = edge.from->p;
    const Point b = edge.to->p;
    const Point ab = b - a;
    const coord_t ab_size = ab.cast<int64_t>().norm();

    const coord_t transition_length = beading_strategy.getTransitioningLength(lower_bead_count);
    const float transition_mid_position = beading_strategy.getTransitionAnchorPos(lower_bead_count);
    constexpr float inner_bead_width_ratio_after_transition = 1.0;

    constexpr coord_t start_rest = 0;
    const float mid_rest = transition_mid_position * inner_bead_width_ratio_after_transition;
    constexpr float end_rest = inner_bead_width_ratio_after_transition;

    { // Lower bead count transition end
        const coord_t start_pos = ab_size - mid_pos;
        const coord_t transition_half_length = transition_mid_position * int64_t(transition_length);
        const coord_t end_pos = start_pos + transition_half_length;
        generateTransitionEnd(*edge.twin, start_pos, end_pos, transition_half_length, mid_rest, start_rest, lower_bead_count, edge_transition_ends);
    }

    { // Upper bead count transition end
        const coord_t start_pos = mid_pos;
        const coord_t transition_half_length = (1.0 - transition_mid_position) * transition_length;
        const coord_t end_pos = mid_pos + transition_half_length;
#ifdef DEBUG
        if (! generateTransitionEnd(edge, start_pos, end_pos, transition_half_length, mid_rest, end_rest, lower_bead_count, edge_transition_ends))
        {
            BOOST_LOG_TRIVIAL(warning) << "There must have been at least one direction in which the bead count is increasing enough for the transition to happen!";
        }
#else
        generateTransitionEnd(edge, start_pos, end_pos, transition_half_length, mid_rest, end_rest, lower_bead_count, edge_transition_ends);
#endif
    }
}

bool SkeletalTrapezoidation::generateTransitionEnd(edge_t& edge, coord_t start_pos, coord_t end_pos, coord_t transition_half_length, double start_rest, double end_rest, coord_t lower_bead_count, ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    Point a = edge.from->p;
    Point b = edge.to->p;
    Point ab = b - a;
    coord_t ab_size = ab.cast<int64_t>().norm(); // TODO: prevent recalculation of these values

    assert(start_pos <= ab_size);
    if(start_pos > ab_size)
    {
        BOOST_LOG_TRIVIAL(warning) << "Start position of edge is beyond edge range.";
    }

    bool going_up = end_rest > start_rest;

    assert(edge.data.isCentral());
    if (!edge.data.isCentral())
    {
        BOOST_LOG_TRIVIAL(warning) << "This function shouldn't generate ends in or beyond non-central regions.";
        return false;
    }

    if (end_pos > ab_size)
    { // Recurse on all further edges
        float rest = end_rest - (start_rest - end_rest) * (end_pos - ab_size) / (start_pos - end_pos);
        assert(rest >= 0);
        assert(rest <= std::max(end_rest, start_rest));
        assert(rest >= std::min(end_rest, start_rest));

        coord_t central_edge_count = 0;
        for (edge_t* outgoing = edge.next; outgoing && outgoing != edge.twin; outgoing = outgoing->twin->next)
        {
            if (!outgoing->data.isCentral()) continue;
            central_edge_count++;
        }

        bool is_only_going_down = true;
        bool has_recursed = false;
        for (edge_t* outgoing = edge.next; outgoing && outgoing != edge.twin;)
        {
            edge_t* next = outgoing->twin->next; // Before we change the outgoing edge itself
            if (!outgoing->data.isCentral())
            {
                outgoing = next;
                continue; // Don't put transition ends in non-central regions
            }
            if (central_edge_count > 1 && going_up && isGoingDown(outgoing, 0, end_pos - ab_size + transition_half_length, lower_bead_count))
            { // We're after a 3-way_all-central_junction-node and going in the direction of lower bead count
                // don't introduce a transition end along this central direction, because this direction is the downward direction
                // while we are supposed to be [going_up]
                outgoing = next;
                continue;
            }
            bool is_going_down = generateTransitionEnd(*outgoing, 0, end_pos - ab_size, transition_half_length, rest, end_rest, lower_bead_count, edge_transition_ends);
            is_only_going_down &= is_going_down;
            outgoing = next;
            has_recursed = true;
        }
        if (!going_up || (has_recursed && !is_only_going_down))
        {
            edge.to->data.transition_ratio = rest;
            edge.to->data.bead_count = lower_bead_count;
        }
        return is_only_going_down;
    }
    else // end_pos < ab_size
    { // Add transition end point here
        bool is_lower_end = end_rest == 0; // TODO collapse this parameter into the bool for which it is used here!
        coord_t pos = -1;

        edge_t* upward_edge = nullptr;
        if (edge.isUpward())
        {
            upward_edge = &edge;
            pos = end_pos;
        }
        else
        {
            upward_edge = edge.twin;
            pos = ab_size - end_pos;
        }

        if(!upward_edge->data.hasTransitionEnds())
        {
            //This edge doesn't have a data structure yet for the transition ends. Make one.
            edge_transition_ends.emplace_back(std::make_shared<std::list<TransitionEnd>>());
            upward_edge->data.setTransitionEnds(edge_transition_ends.back());
        }
        auto transitions = upward_edge->data.getTransitionEnds();

        //Add a transition to it (on the correct side).
        assert(ab_size == (edge.twin->from->p - edge.twin->to->p).cast<int64_t>().norm());
        assert(pos <= ab_size);
        if (transitions->empty() || pos < transitions->front().pos)
        { // Preorder so that sorting later on is faster
            transitions->emplace_front(pos, lower_bead_count, is_lower_end);
        }
        else
        {
            transitions->emplace_back(pos, lower_bead_count, is_lower_end);
        }
        return false;
    }
}


bool SkeletalTrapezoidation::isGoingDown(edge_t* outgoing, coord_t traveled_dist, coord_t max_dist, coord_t lower_bead_count) const
{
    // NOTE: the logic below is not fully thought through.
    // TODO: take transition mids into account
    if (outgoing->to->data.distance_to_boundary == 0)
    {
        return true;
    }
    bool is_upward = outgoing->to->data.distance_to_boundary >= outgoing->from->data.distance_to_boundary;
    edge_t* upward_edge = is_upward? outgoing : outgoing->twin;
    if (outgoing->to->data.bead_count > lower_bead_count + 1)
    {
        assert(upward_edge->data.hasTransitions() && "If the bead count is going down there has to be a transition mid!");
        if(!upward_edge->data.hasTransitions())
        {
            BOOST_LOG_TRIVIAL(warning) << "If the bead count is going down there has to be a transition mid!";
        }
        return false;
    }
    coord_t length = (outgoing->to->p - outgoing->from->p).cast<int64_t>().norm();
    if (upward_edge->data.hasTransitions())
    {
        auto& transition_mids = *upward_edge->data.getTransitions();
        TransitionMiddle& mid = is_upward? transition_mids.front() : transition_mids.back();
        if (
            mid.lower_bead_count == lower_bead_count &&
            ((is_upward && mid.pos + traveled_dist < max_dist)
                || (!is_upward && length - mid.pos + traveled_dist < max_dist))
        )
        {
            return true;
        }
    }
    if (traveled_dist + length > max_dist)
    {
        return false;
    }
    if (outgoing->to->data.bead_count <= lower_bead_count
        && !(outgoing->to->data.bead_count == lower_bead_count && outgoing->to->data.transition_ratio > 0.0))
    {
        return true;
    }
    
    bool is_only_going_down = true;
    bool has_recursed = false;
    for (edge_t* next = outgoing->next; next && next != outgoing->twin; next = next->twin->next)
    {
        if (!next->data.isCentral())
        {
            continue;
        }
        bool is_going_down = isGoingDown(next, traveled_dist + length, max_dist, lower_bead_count);
        is_only_going_down &= is_going_down;
        has_recursed = true;
    }
    return has_recursed && is_only_going_down;
}

static inline Point normal(const Point& p0, coord_t len)
{
    int64_t _len = p0.cast<int64_t>().norm();
    if (_len < 1)
        return Point(len, 0);
    return (p0.cast<int64_t>() * int64_t(len) / _len).cast<coord_t>();
};

void SkeletalTrapezoidation::applyTransitions(ptr_vector_t<std::list<TransitionEnd>>& edge_transition_ends)
{
    const auto _snap_dist = snap_dist();
    for (edge_t& edge : graph.edges)
    {
        if (edge.twin->data.hasTransitionEnds())
        {
            coord_t length = (edge.from->p - edge.to->p).cast<int64_t>().norm();
            auto& twin_transition_ends = *edge.twin->data.getTransitionEnds();
            if (! edge.data.hasTransitionEnds())
            {
                edge_transition_ends.emplace_back(std::make_shared<std::list<TransitionEnd>>());
                edge.data.setTransitionEnds(edge_transition_ends.back());
            }
            auto& transition_ends = *edge.data.getTransitionEnds();
            for (TransitionEnd& end : twin_transition_ends)
            {
                transition_ends.emplace_back(length - end.pos, end.lower_bead_count, end.is_lower_end);
            }
            twin_transition_ends.clear();
        }
    }
    
    for (edge_t& edge : graph.edges)
    {
        if (! edge.data.hasTransitionEnds())
        {
            continue;
        }

        assert(edge.data.isCentral());

        auto& transitions = *edge.data.getTransitionEnds();
        transitions.sort([](const TransitionEnd& a, const TransitionEnd& b) { return a.pos < b.pos; } );

        node_t* from = edge.from;
        node_t* to = edge.to;
        Point a = from->p;
        Point b = to->p;
        Point ab = b - a;
        coord_t ab_size = (ab).cast<int64_t>().norm();

        edge_t* last_edge_replacing_input = &edge;
        for (TransitionEnd& transition_end : transitions)
        {
            coord_t new_node_bead_count = transition_end.is_lower_end? transition_end.lower_bead_count : transition_end.lower_bead_count + 1;
            coord_t end_pos = transition_end.pos;
            node_t* close_node = (end_pos < ab_size / 2)? from : to;
            if ((end_pos < _snap_dist || end_pos > ab_size - _snap_dist)
                && close_node->data.bead_count == new_node_bead_count
            )
            {
                assert(end_pos <= ab_size);
                close_node->data.transition_ratio = 0;
                continue;
            }
            Point mid = a + normal(ab, end_pos);
            
            assert(last_edge_replacing_input->data.isCentral());
            assert(last_edge_replacing_input->data.type != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            last_edge_replacing_input = graph.insertNode(last_edge_replacing_input, mid, new_node_bead_count);
            assert(last_edge_replacing_input->data.type != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            assert(last_edge_replacing_input->data.isCentral());
        }
    }
}

bool SkeletalTrapezoidation::isEndOfCentral(const edge_t& edge_to) const
{
    if (!edge_to.data.isCentral())
    {
        return false;
    }
    if (!edge_to.next)
    {
        return true;
    }
    for (const edge_t* edge = edge_to.next; edge && edge != edge_to.twin; edge = edge->twin->next)
    {
        if (edge->data.isCentral())
        {
            return false;
        }
        assert(edge->twin);
    }
    return true;
}

void SkeletalTrapezoidation::generateExtraRibs()
{
    const auto _snap_dist = snap_dist();
    for (auto edge_it = graph.edges.begin(); edge_it != graph.edges.end(); ++edge_it)
    {
        edge_t& edge = *edge_it;
        
        if (!edge.data.isCentral() 
            || shorter_then(edge.to->p - edge.from->p, discretization_step_size)
            || edge.from->data.distance_to_boundary >= edge.to->data.distance_to_boundary) 
        {
            continue;
        }


        std::vector<coord_t> rib_thicknesses = beading_strategy.getNonlinearThicknesses(edge.from->data.bead_count);

        if (rib_thicknesses.empty()) continue;

        // Preload some variables before [edge] gets changed
        node_t* from = edge.from;
        node_t* to = edge.to;
        Point a = from->p;
        Point b = to->p;
        Point ab = b - a;
        coord_t ab_size = ab.cast<int64_t>().norm();
        coord_t a_R = edge.from->data.distance_to_boundary;
        coord_t b_R = edge.to->data.distance_to_boundary;
        
        edge_t* last_edge_replacing_input = &edge;
        for (coord_t rib_thickness : rib_thicknesses)
        {
            if (rib_thickness / 2 <= a_R) 
            {
                continue;
            }
            if (rib_thickness / 2 >= b_R) 
            {
                break;
            }
            
            coord_t new_node_bead_count = std::min(edge.from->data.bead_count, edge.to->data.bead_count);
            coord_t end_pos = int64_t(ab_size) * int64_t(rib_thickness / 2 - a_R) / int64_t(b_R - a_R);
            assert(end_pos > 0);
            assert(end_pos < ab_size);
            node_t* close_node = (end_pos < ab_size / 2)? from : to;
            if ((end_pos < _snap_dist || end_pos > ab_size - _snap_dist)
                && close_node->data.bead_count == new_node_bead_count
            )
            {
                assert(end_pos <= ab_size);
                close_node->data.transition_ratio = 0;
                continue;
            }
            Point mid = a + normal(ab, end_pos);
            
            assert(last_edge_replacing_input->data.isCentral());
            assert(last_edge_replacing_input->data.type != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            last_edge_replacing_input = graph.insertNode(last_edge_replacing_input, mid, new_node_bead_count);
            assert(last_edge_replacing_input->data.type != SkeletalTrapezoidationEdge::EdgeType::EXTRA_VD);
            assert(last_edge_replacing_input->data.isCentral());
        }
    }
}

//
// ^^^^^^^^^^^^^^^^^^^^^
//    TRANSTISIONING
// =====================
//  TOOLPATH GENERATION
// vvvvvvvvvvvvvvvvvvvvv
//

void SkeletalTrapezoidation::generateSegments()
{
    std::vector<edge_t*> upward_quad_mids;
    for (edge_t& edge : graph.edges)
    {
        if (edge.prev && edge.next && edge.isUpward())
        {
            upward_quad_mids.emplace_back(&edge);
        }
    }
    
    std::sort(upward_quad_mids.begin(), upward_quad_mids.end(), [](edge_t* a, edge_t* b)
    {
        if (a->to->data.distance_to_boundary == b->to->data.distance_to_boundary)
        { // Ordering between two 'upward' edges of the same distance is important when one of the edges is flat and connected to the other
            if (a->from->data.distance_to_boundary == a->to->data.distance_to_boundary
                && b->from->data.distance_to_boundary == b->to->data.distance_to_boundary)
            {
                coord_t max = std::numeric_limits<coord_t>::max();
                coord_t a_dist_from_up = std::min(a->distToGoUp().value_or(max), a->twin->distToGoUp().value_or(max)) - (a->to->p - a->from->p).cast<int64_t>().norm();
                coord_t b_dist_from_up = std::min(b->distToGoUp().value_or(max), b->twin->distToGoUp().value_or(max)) - (b->to->p - b->from->p).cast<int64_t>().norm();
                return a_dist_from_up < b_dist_from_up;
            }
            else if (a->from->data.distance_to_boundary == a->to->data.distance_to_boundary)
            {
                return true; // Edge a might be 'above' edge b
            }
            else if (b->from->data.distance_to_boundary == b->to->data.distance_to_boundary)
            {
                return false; // Edge b might be 'above' edge a
            }
            else
            {
                // Ordering is not important
            }
        }
        return a->to->data.distance_to_boundary > b->to->data.distance_to_boundary;
    });

    ptr_vector_t<BeadingPropagation> node_beadings;
    { // Store beading
        for (node_t& node : graph.nodes)
        {
            if (node.data.bead_count <= 0)
            {
                continue;
            }
            if (node.data.transition_ratio == 0)
            {
                node_beadings.emplace_back(new BeadingPropagation(beading_strategy.compute(node.data.distance_to_boundary * 2, node.data.bead_count)));
                node.data.setBeading(node_beadings.back());
                assert(node_beadings.back()->beading.total_thickness == node.data.distance_to_boundary * 2);
                if(node_beadings.back()->beading.total_thickness != node.data.distance_to_boundary * 2)
                {
                    BOOST_LOG_TRIVIAL(warning) << "If transitioning to an endpoint (ratio 0), the node should be exactly in the middle.";
                }
            }
            else
            {
                Beading low_count_beading = beading_strategy.compute(node.data.distance_to_boundary * 2, node.data.bead_count);
                Beading high_count_beading = beading_strategy.compute(node.data.distance_to_boundary * 2, node.data.bead_count + 1);
                Beading merged = interpolate(low_count_beading, 1.0 - node.data.transition_ratio, high_count_beading);
                node_beadings.emplace_back(new BeadingPropagation(merged));
                node.data.setBeading(node_beadings.back());
                assert(merged.total_thickness == node.data.distance_to_boundary * 2);
                if(merged.total_thickness != node.data.distance_to_boundary * 2)
                {
                    BOOST_LOG_TRIVIAL(warning) << "If merging two beads, the new bead must be exactly in the middle.";
                }
            }
        }
    }

#ifdef ARACHNE_DEBUG
    static int iRun = 0;
    export_graph_to_svg(debug_out_path("ST-generateSegments-before-propagation-%d.svg", iRun), this->graph, this->outline);
#endif

    propagateBeadingsUpward(upward_quad_mids, node_beadings);

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateSegments-upward-propagation-%d.svg", iRun), this->graph, this->outline);
#endif

    propagateBeadingsDownward(upward_quad_mids, node_beadings);

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateSegments-downward-propagation-%d.svg", iRun), this->graph, this->outline);
#endif

    ptr_vector_t<LineJunctions> edge_junctions; // junctions ordered high R to low R
    generateJunctions(node_beadings, edge_junctions);

#ifdef ARACHNE_DEBUG
    export_graph_to_svg(debug_out_path("ST-generateSegments-junctions-%d.svg", iRun), this->graph, this->outline, edge_junctions);
#endif

    connectJunctions(edge_junctions);

    generateLocalMaximaSingleBeads();

#ifdef ARACHNE_DEBUG
    ++iRun;
#endif
}

SkeletalTrapezoidation::edge_t* SkeletalTrapezoidation::getQuadMaxRedgeTo(edge_t* quad_start_edge)
{
    assert(quad_start_edge->prev == nullptr);
    assert(quad_start_edge->from->data.distance_to_boundary == 0);
    coord_t max_R = -1;
    edge_t* ret = nullptr;
    for (edge_t* edge = quad_start_edge; edge; edge = edge->next)
    {
        coord_t r = edge->to->data.distance_to_boundary;
        if (r > max_R)
        {
            max_R = r;
            ret = edge;
        }
    }

    if (!ret->next && ret->to->data.distance_to_boundary - scaled<coord_t>(0.005) < ret->from->data.distance_to_boundary)
    {
        ret = ret->prev;
    }
    assert(ret);
    assert(ret->next);
    return ret;
}

void SkeletalTrapezoidation::propagateBeadingsUpward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    const auto _central_filter_dist = central_filter_dist();
    for (auto upward_quad_mids_it = upward_quad_mids.rbegin(); upward_quad_mids_it != upward_quad_mids.rend(); ++upward_quad_mids_it)
    {
        edge_t* upward_edge = *upward_quad_mids_it;
        if (upward_edge->to->data.bead_count >= 0)
        { // Don't override local beading
            continue;
        }
        if (! upward_edge->from->data.hasBeading())
        { // Only propagate if we have something to propagate
            continue;
        }
        BeadingPropagation& lower_beading = *upward_edge->from->data.getBeading();
        if (upward_edge->to->data.hasBeading())
        { // Only propagate to places where there is place
            continue;
        }
        assert((upward_edge->from->data.distance_to_boundary != upward_edge->to->data.distance_to_boundary || shorter_then(upward_edge->to->p - upward_edge->from->p, _central_filter_dist)) && "zero difference R edges should always be central");
        coord_t length = (upward_edge->to->p - upward_edge->from->p).cast<int64_t>().norm();
        BeadingPropagation upper_beading = lower_beading;
        upper_beading.dist_to_bottom_source += length;
        upper_beading.is_upward_propagated_only = true;
        node_beadings.emplace_back(new BeadingPropagation(upper_beading));
        upward_edge->to->data.setBeading(node_beadings.back());
        assert(upper_beading.beading.total_thickness <= upward_edge->to->data.distance_to_boundary * 2);
    }
}

void SkeletalTrapezoidation::propagateBeadingsDownward(std::vector<edge_t*>& upward_quad_mids, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    for (edge_t* upward_quad_mid : upward_quad_mids)
    {
        // Transfer beading information to lower nodes
        if (!upward_quad_mid->data.isCentral())
        {
            // for equidistant edge: propagate from known beading to node with unknown beading
            if (upward_quad_mid->from->data.distance_to_boundary == upward_quad_mid->to->data.distance_to_boundary
                && upward_quad_mid->from->data.hasBeading()
                && ! upward_quad_mid->to->data.hasBeading()
            )
            {
                propagateBeadingsDownward(upward_quad_mid->twin, node_beadings);
            }
            else
            {
                propagateBeadingsDownward(upward_quad_mid, node_beadings);
            }
        }
    }
}

void SkeletalTrapezoidation::propagateBeadingsDownward(edge_t* edge_to_peak, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    coord_t length = (edge_to_peak->to->p - edge_to_peak->from->p).cast<int64_t>().norm();
    BeadingPropagation& top_beading = *getOrCreateBeading(edge_to_peak->to, node_beadings);
    assert(top_beading.beading.total_thickness >= edge_to_peak->to->data.distance_to_boundary * 2);
    if(top_beading.beading.total_thickness < edge_to_peak->to->data.distance_to_boundary * 2)
    {
        BOOST_LOG_TRIVIAL(warning) << "Top bead is beyond the center of the total width.";
    }
    assert(!top_beading.is_upward_propagated_only);

    if(!edge_to_peak->from->data.hasBeading())
    { // Set new beading if there is no beading associated with the node yet
        BeadingPropagation propagated_beading = top_beading;
        propagated_beading.dist_from_top_source += length;
        node_beadings.emplace_back(new BeadingPropagation(propagated_beading));
        edge_to_peak->from->data.setBeading(node_beadings.back());
        assert(propagated_beading.beading.total_thickness >= edge_to_peak->from->data.distance_to_boundary * 2);
        if(propagated_beading.beading.total_thickness < edge_to_peak->from->data.distance_to_boundary * 2)
        {
            BOOST_LOG_TRIVIAL(warning) << "Propagated bead is beyond the center of the total width.";
        }
    }
    else
    {
        BeadingPropagation& bottom_beading = *edge_to_peak->from->data.getBeading();
        coord_t total_dist = top_beading.dist_from_top_source + length + bottom_beading.dist_to_bottom_source;
        double ratio_of_top = static_cast<float>(bottom_beading.dist_to_bottom_source) / std::min(total_dist, beading_propagation_transition_dist);
        ratio_of_top = std::max(0.0, ratio_of_top);
        if (ratio_of_top >= 1.0)
        {
            bottom_beading = top_beading;
            bottom_beading.dist_from_top_source += length;
        }
        else
        {
            Beading merged_beading = interpolate(top_beading.beading, ratio_of_top, bottom_beading.beading, edge_to_peak->from->data.distance_to_boundary);
            bottom_beading = BeadingPropagation(merged_beading);
            bottom_beading.is_upward_propagated_only = false;
            assert(merged_beading.total_thickness >= edge_to_peak->from->data.distance_to_boundary * 2);
            if(merged_beading.total_thickness < edge_to_peak->from->data.distance_to_boundary * 2)
            {
                BOOST_LOG_TRIVIAL(warning) << "Merged bead is beyond the center of the total width.";
            }
        }
    }
}


SkeletalTrapezoidation::Beading SkeletalTrapezoidation::interpolate(const Beading& left, double ratio_left_to_whole, const Beading& right, coord_t switching_radius) const
{
    assert(ratio_left_to_whole >= 0.0 && ratio_left_to_whole <= 1.0);
    Beading ret = interpolate(left, ratio_left_to_whole, right);

    // TODO: don't use toolpath locations past the middle!
    // TODO: stretch bead widths and locations of the higher bead count beading to fit in the left over space
    coord_t next_inset_idx;
    for (next_inset_idx = left.toolpath_locations.size() - 1; next_inset_idx >= 0; next_inset_idx--)
    {
        if (switching_radius > left.toolpath_locations[next_inset_idx])
        {
            break;
        }
    }
    if (next_inset_idx < 0)
    { // There is no next inset, because there is only one
        assert(left.toolpath_locations.empty() || left.toolpath_locations.front() >= switching_radius);
        return ret;
    }
    if (next_inset_idx + 1 == coord_t(left.toolpath_locations.size()))
    { // We cant adjust to fit the next edge because there is no previous one?!
        return ret;
    }
    assert(next_inset_idx < coord_t(left.toolpath_locations.size()));
    assert(left.toolpath_locations[next_inset_idx] <= switching_radius);
    assert(left.toolpath_locations[next_inset_idx + 1] >= switching_radius);
    if (ret.toolpath_locations[next_inset_idx] > switching_radius)
    { // One inset disappeared between left and the merged one
        // solve for ratio f:
        // f*l + (1-f)*r = s
        // f*l + r - f*r = s
        // f*(l-r) + r = s
        // f*(l-r) = s - r
        // f = (s-r) / (l-r)
        float new_ratio = static_cast<float>(switching_radius - right.toolpath_locations[next_inset_idx]) / static_cast<float>(left.toolpath_locations[next_inset_idx] - right.toolpath_locations[next_inset_idx]);
        new_ratio = std::min(1.0, new_ratio + 0.1);
        return interpolate(left, new_ratio, right);
    }
    return ret;
}


SkeletalTrapezoidation::Beading SkeletalTrapezoidation::interpolate(const Beading& left, double ratio_left_to_whole, const Beading& right) const
{
    assert(ratio_left_to_whole >= 0.0 && ratio_left_to_whole <= 1.0);
    float ratio_right_to_whole = 1.0 - ratio_left_to_whole;

    Beading ret = (left.total_thickness > right.total_thickness)? left : right;
    for (size_t inset_idx = 0; inset_idx < std::min(left.bead_widths.size(), right.bead_widths.size()); inset_idx++)
    {
        if(left.bead_widths[inset_idx] == 0 || right.bead_widths[inset_idx] == 0)
        {
            ret.bead_widths[inset_idx] = 0; //0-width wall markers stay 0-width.
        }
        else
        {
            ret.bead_widths[inset_idx] = ratio_left_to_whole * left.bead_widths[inset_idx] + ratio_right_to_whole * right.bead_widths[inset_idx];
        }
        ret.toolpath_locations[inset_idx] = ratio_left_to_whole * left.toolpath_locations[inset_idx] + ratio_right_to_whole * right.toolpath_locations[inset_idx];
    }
    return ret;
}

void SkeletalTrapezoidation::generateJunctions(ptr_vector_t<BeadingPropagation>& node_beadings, ptr_vector_t<LineJunctions>& edge_junctions)
{
    for (edge_t& edge_ : graph.edges)
    {
        edge_t* edge = &edge_;
        if (edge->from->data.distance_to_boundary > edge->to->data.distance_to_boundary)
        { // Only consider the upward half-edges
            continue;
        }

        coord_t start_R = edge->to->data.distance_to_boundary; // higher R
        coord_t end_R = edge->from->data.distance_to_boundary; // lower R

        if ((edge->from->data.bead_count == edge->to->data.bead_count && edge->from->data.bead_count >= 0)
            || end_R >= start_R)
        { // No beads to generate
            continue;
        }

        Beading* beading = &getOrCreateBeading(edge->to, node_beadings)->beading;
        edge_junctions.emplace_back(std::make_shared<LineJunctions>());
        edge_.data.setExtrusionJunctions(edge_junctions.back());  // initialization
        LineJunctions& ret = *edge_junctions.back();

        assert(beading->total_thickness >= edge->to->data.distance_to_boundary * 2);
        if(beading->total_thickness < edge->to->data.distance_to_boundary * 2)
        {
            BOOST_LOG_TRIVIAL(warning) << "Generated junction is beyond the center of total width.";
        }

        Point a = edge->to->p;
        Point b = edge->from->p;
        Point ab = b - a;

        const size_t num_junctions = beading->toolpath_locations.size();
        size_t junction_idx;
        // Compute starting junction_idx for this segment
        for (junction_idx = (std::max(size_t(1), beading->toolpath_locations.size()) - 1) / 2; junction_idx < num_junctions; junction_idx--)
        {
            coord_t bead_R = beading->toolpath_locations[junction_idx];
            // toolpath_locations computed inside DistributedBeadingStrategy could be off by 1 because of rounding errors.
            // In GH issue #8472, these roundings errors caused missing the middle extrusion.
            // Adding small epsilon should help resolve those cases.
            if (bead_R <= start_R + 1)
            { // Junction coinciding with start node is used in this function call
                break;
            }
        }

        // Robustness against odd segments which might lie just slightly outside of the range due to rounding errors
        // not sure if this is really needed (TODO)
        if (junction_idx + 1 < num_junctions
            && beading->toolpath_locations[junction_idx + 1] <= start_R + scaled<coord_t>(0.005)
            && beading->total_thickness < start_R + scaled<coord_t>(0.005)
        )
        {
            junction_idx++;
        }

        for (; junction_idx < num_junctions; junction_idx--) //When junction_idx underflows, it'll be more than num_junctions too.
        {
            coord_t bead_R = beading->toolpath_locations[junction_idx];
            assert(bead_R >= 0);
            if (bead_R < end_R)
            { // Junction coinciding with a node is handled by the next segment
                break;
            }
            Point junction(a + (ab.cast<int64_t>() * int64_t(bead_R - start_R) / int64_t(end_R - start_R)).cast<coord_t>());
            if (bead_R > start_R - scaled<coord_t>(0.005))
            { // Snap to start node if it is really close, in order to be able to see 3-way intersection later on more robustly
                junction = a;
            }
            ret.emplace_back(junction, beading->bead_widths[junction_idx], junction_idx);
        }
    }
}

std::shared_ptr<SkeletalTrapezoidationJoint::BeadingPropagation> SkeletalTrapezoidation::getOrCreateBeading(node_t* node, ptr_vector_t<BeadingPropagation>& node_beadings)
{
    if (! node->data.hasBeading())
    {
        if (node->data.bead_count == -1)
        { // This bug is due to too small central edges
            const coord_t nearby_dist = scaled<coord_t>(0.1);
            auto nearest_beading = getNearestBeading(node, nearby_dist);
            if (nearest_beading)
            {
                return nearest_beading;
            }
            
            // Else make a new beading:
            bool has_central_edge = false;
            bool first = true;
            coord_t dist = std::numeric_limits<coord_t>::max();
            for (edge_t* edge = node->incident_edge; edge && (first || edge != node->incident_edge); edge = edge->twin->next)
            {
                if (edge->data.isCentral())
                {
                    has_central_edge = true;
                }
                assert(edge->to->data.distance_to_boundary >= 0);
                dist = std::min(dist, edge->to->data.distance_to_boundary + coord_t((edge->to->p - edge->from->p).cast<int64_t>().norm()));
                first = false;
            }
            if (!has_central_edge)
            {
                BOOST_LOG_TRIVIAL(error) << "Unknown beading for non-central node!";
            }
            assert(dist != std::numeric_limits<coord_t>::max());
            node->data.bead_count = beading_strategy.getOptimalBeadCount(dist * 2);
        }
        assert(node->data.bead_count != -1);
        node_beadings.emplace_back(new BeadingPropagation(beading_strategy.compute(node->data.distance_to_boundary * 2, node->data.bead_count)));
        node->data.setBeading(node_beadings.back());
    }
    assert(node->data.hasBeading());
    return node->data.getBeading();
}

std::shared_ptr<SkeletalTrapezoidationJoint::BeadingPropagation> SkeletalTrapezoidation::getNearestBeading(node_t* node, coord_t max_dist)
{
    struct DistEdge
    {
        edge_t* edge_to;
        coord_t dist;
        DistEdge(edge_t* edge_to, coord_t dist)
        : edge_to(edge_to), dist(dist)
        {}
    };

    auto compare = [](const DistEdge& l, const DistEdge& r) -> bool { return l.dist > r.dist; };
    std::priority_queue<DistEdge, std::vector<DistEdge>, decltype(compare)> further_edges(compare);
    bool first = true;
    for (edge_t* outgoing = node->incident_edge; outgoing && (first || outgoing != node->incident_edge); outgoing = outgoing->twin->next)
    {
        further_edges.emplace(outgoing, (outgoing->to->p - outgoing->from->p).cast<int64_t>().norm());
        first = false;
    }

    for (coord_t counter = 0; counter < SKELETAL_TRAPEZOIDATION_BEAD_SEARCH_MAX; counter++)
    { // Prevent endless recursion
        if (further_edges.empty()) return nullptr;
        DistEdge here = further_edges.top();
        further_edges.pop();
        if (here.dist > max_dist) return nullptr;
        if (here.edge_to->to->data.hasBeading())
        {
            return here.edge_to->to->data.getBeading();
        }
        else
        { // recurse
            for (edge_t* further_edge = here.edge_to->next; further_edge && further_edge != here.edge_to->twin; further_edge = further_edge->twin->next)
            {
                further_edges.emplace(further_edge, here.dist + (further_edge->to->p - further_edge->from->p).cast<int64_t>().norm());
            }
        }
    }
    return nullptr;
}

void SkeletalTrapezoidation::addToolpathSegment(const ExtrusionJunction& from, const ExtrusionJunction& to, bool is_odd, bool force_new_path, bool from_is_3way, bool to_is_3way)
{
    if (from == to) return;

    std::vector<VariableWidthLines> &generated_toolpaths = *p_generated_toolpaths;

    size_t inset_idx = from.perimeter_index;
    if (inset_idx >= generated_toolpaths.size())
    {
        generated_toolpaths.resize(inset_idx + 1);
    }
    assert((generated_toolpaths[inset_idx].empty() || !generated_toolpaths[inset_idx].back().junctions.empty()) && "empty extrusion lines should never have been generated");
    if (generated_toolpaths[inset_idx].empty()
        || generated_toolpaths[inset_idx].back().is_odd != is_odd
        || generated_toolpaths[inset_idx].back().junctions.back().perimeter_index != inset_idx // inset_idx should always be consistent
    )
    {
        force_new_path = true;
    }
    if (!force_new_path
        && shorter_then(generated_toolpaths[inset_idx].back().junctions.back().p - from.p, scaled<coord_t>(0.010))
        && std::abs(generated_toolpaths[inset_idx].back().junctions.back().w - from.w) < scaled<coord_t>(0.010)
        && ! from_is_3way // force new path at 3way intersection
    )
    {
        generated_toolpaths[inset_idx].back().junctions.push_back(to);
    }
    else if (!force_new_path
             && shorter_then(generated_toolpaths[inset_idx].back().junctions.back().p - to.p, scaled<coord_t>(0.010))
             && std::abs(generated_toolpaths[inset_idx].back().junctions.back().w - to.w) < scaled<coord_t>(0.010)
             && ! to_is_3way // force new path at 3way intersection
    )
    {
        if ( ! is_odd)
        {
            BOOST_LOG_TRIVIAL(error) << "Reversing even wall line causes it to be printed CCW instead of CW!";
        }
        generated_toolpaths[inset_idx].back().junctions.push_back(from);
    }
    else
    {
        generated_toolpaths[inset_idx].emplace_back(inset_idx, is_odd);
        generated_toolpaths[inset_idx].back().junctions.push_back(from);
        generated_toolpaths[inset_idx].back().junctions.push_back(to);
    }
};

void SkeletalTrapezoidation::connectJunctions(ptr_vector_t<LineJunctions>& edge_junctions)
{
    using EdgeSet = ankerl::unordered_dense::set<edge_t*>;

    EdgeSet unprocessed_quad_starts(graph.edges.size() * 5 / 2);
    for (edge_t& edge : graph.edges)
    {
        if (!edge.prev)
        {
            unprocessed_quad_starts.emplace(&edge);
        }
    }

    EdgeSet passed_odd_edges;

    while (!unprocessed_quad_starts.empty())
    {
        edge_t* poly_domain_start = *unprocessed_quad_starts.begin();
        edge_t* quad_start = poly_domain_start;
        bool new_domain_start = true;
        do
        {
            edge_t* quad_end = quad_start;
            while (quad_end->next)
            {
                quad_end = quad_end->next;
            }

            edge_t* edge_to_peak = getQuadMaxRedgeTo(quad_start);
            // walk down on both sides and connect junctions
            edge_t* edge_from_peak = edge_to_peak->next; assert(edge_from_peak);

            unprocessed_quad_starts.erase(quad_start);

            if (! edge_to_peak->data.hasExtrusionJunctions())
            {
                edge_junctions.emplace_back(std::make_shared<LineJunctions>());
                edge_to_peak->data.setExtrusionJunctions(edge_junctions.back());
            }
            // The junctions on the edge(s) from the start of the quad to the node with highest R
            LineJunctions from_junctions = *edge_to_peak->data.getExtrusionJunctions();
            if (! edge_from_peak->twin->data.hasExtrusionJunctions())
            {
                edge_junctions.emplace_back(std::make_shared<LineJunctions>());
                edge_from_peak->twin->data.setExtrusionJunctions(edge_junctions.back());
            }
            // The junctions on the edge(s) from the end of the quad to the node with highest R
            LineJunctions to_junctions = *edge_from_peak->twin->data.getExtrusionJunctions();
            if (edge_to_peak->prev)
            {
                LineJunctions from_prev_junctions = *edge_to_peak->prev->data.getExtrusionJunctions();
                while (!from_junctions.empty() && !from_prev_junctions.empty() && from_junctions.back().perimeter_index <= from_prev_junctions.front().perimeter_index)
                {
                    from_junctions.pop_back();
                }
                from_junctions.reserve(from_junctions.size() + from_prev_junctions.size());
                from_junctions.insert(from_junctions.end(), from_prev_junctions.begin(), from_prev_junctions.end());
                assert(!edge_to_peak->prev->prev);
                if(edge_to_peak->prev->prev)
                {
                    BOOST_LOG_TRIVIAL(warning) << "The edge we're about to connect is already connected.";
                }
            }
            if (edge_from_peak->next)
            {
                LineJunctions to_next_junctions = *edge_from_peak->next->twin->data.getExtrusionJunctions();
                while (!to_junctions.empty() && !to_next_junctions.empty() && to_junctions.back().perimeter_index <= to_next_junctions.front().perimeter_index)
                {
                    to_junctions.pop_back();
                }
                to_junctions.reserve(to_junctions.size() + to_next_junctions.size());
                to_junctions.insert(to_junctions.end(), to_next_junctions.begin(), to_next_junctions.end());
                assert(!edge_from_peak->next->next);
                if(edge_from_peak->next->next)
                {
                    BOOST_LOG_TRIVIAL(warning) << "The edge we're about to connect is already connected!";
                }
            }
            assert(std::abs(int(from_junctions.size()) - int(to_junctions.size())) <= 1); // at transitions one end has more beads
            if(std::abs(int(from_junctions.size()) - int(to_junctions.size())) > 1)
            {
                BOOST_LOG_TRIVIAL(warning) << "Can't create a transition when connecting two perimeters where the number of beads differs too much! " << from_junctions.size() << " vs. " << to_junctions.size();
            }

            size_t segment_count = std::min(from_junctions.size(), to_junctions.size());
            for (size_t junction_rev_idx = 0; junction_rev_idx < segment_count; junction_rev_idx++)
            {
                ExtrusionJunction& from = from_junctions[from_junctions.size() - 1 - junction_rev_idx];
                ExtrusionJunction& to = to_junctions[to_junctions.size() - 1 - junction_rev_idx];
                assert(from.perimeter_index == to.perimeter_index);
                if(from.perimeter_index != to.perimeter_index)
                {
                    BOOST_LOG_TRIVIAL(warning) << "Connecting two perimeters with different indices! Perimeter " << from.perimeter_index << " and " << to.perimeter_index;
                }
                const bool from_is_odd =
                    quad_start->to->data.bead_count > 0 && quad_start->to->data.bead_count % 2 == 1 // quad contains single bead segment
                    && quad_start->to->data.transition_ratio == 0 // We're not in a transition
                    && junction_rev_idx == segment_count - 1 // Is single bead segment
                    && shorter_then(from.p - quad_start->to->p, scaled<coord_t>(0.005));
                const bool to_is_odd =
                    quad_end->from->data.bead_count > 0 && quad_end->from->data.bead_count % 2 == 1 // quad contains single bead segment
                    && quad_end->from->data.transition_ratio == 0 // We're not in a transition
                    && junction_rev_idx == segment_count - 1 // Is single bead segment
                    && shorter_then(to.p - quad_end->from->p, scaled<coord_t>(0.005));
                const bool is_odd_segment = from_is_odd && to_is_odd;
                if (is_odd_segment
                    && passed_odd_edges.count(quad_start->next->twin) > 0) // Only generate toolpath for odd segments once
                {
                    continue; // Prevent duplication of single bead segments
                }
                bool from_is_3way = from_is_odd && quad_start->to->isMultiIntersection();
                bool to_is_3way = to_is_odd && quad_end->from->isMultiIntersection();
                passed_odd_edges.emplace(quad_start->next);

                addToolpathSegment(from, to, is_odd_segment, new_domain_start, from_is_3way, to_is_3way);
            }
            new_domain_start = false;
        }
        while(quad_start = quad_start->getNextUnconnected(), quad_start != poly_domain_start);
    }
}

void SkeletalTrapezoidation::generateLocalMaximaSingleBeads()
{
    std::vector<VariableWidthLines> &generated_toolpaths = *p_generated_toolpaths;

    for (auto& node : graph.nodes)
    {
        if (! node.data.hasBeading())
        {
            continue;
        }
        Beading& beading = node.data.getBeading()->beading;
        if (beading.bead_widths.size() % 2 == 1 && node.isLocalMaximum(true) && !node.isCentral())
        {
            const size_t inset_index = beading.bead_widths.size() / 2;
            constexpr bool is_odd = true;
            if (inset_index >= generated_toolpaths.size())
            {
                generated_toolpaths.resize(inset_index + 1);
            }
            generated_toolpaths[inset_index].emplace_back(inset_index, is_odd);
            ExtrusionLine& line = generated_toolpaths[inset_index].back();
            const coord_t width = beading.bead_widths[inset_index];
            // total area to be extruded is pi*(w/2)^2 = pi*w*w/4
            // Width a constant extrusion width w, that would be a length of pi*w/4
            // If we make a small circle to fill up the hole, then that circle would have a circumference of 2*pi*r
            // So our circle needs to be such that r=w/8
            const coord_t     r          = width / 8;
            constexpr coord_t n_segments = 6;
            for (coord_t segment = 0; segment < n_segments; segment++) {
                float a = 2.0 * M_PI / n_segments * segment;
                line.junctions.emplace_back(node.p + Point(r * cos(a), r * sin(a)), width, inset_index);
            }
        }
    }
}

//
// ^^^^^^^^^^^^^^^^^^^^^
//  TOOLPATH GENERATION
// =====================
//

} // namespace Slic3r::Arachne
