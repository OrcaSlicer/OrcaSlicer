//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef SKELETAL_TRAPEZOIDATION_GRAPH_H
#define SKELETAL_TRAPEZOIDATION_GRAPH_H

#include <optional>
#include <utility>

#include "utils/HalfEdgeGraph.hpp"
#include "SkeletalTrapezoidationEdge.hpp"
#include "SkeletalTrapezoidationJoint.hpp"
#include "libslic3r/Arachne/utils/HalfEdge.hpp"
#include "libslic3r/Arachne/utils/HalfEdgeNode.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r
{
class Line;
class Point;
};

namespace Slic3r::Arachne
{

class STHalfEdgeNode;

class STHalfEdge : public HalfEdge<SkeletalTrapezoidationJoint, SkeletalTrapezoidationEdge, STHalfEdgeNode, STHalfEdge>
{
    using edge_t = STHalfEdge;
    using node_t = STHalfEdgeNode;
public:
    STHalfEdge(SkeletalTrapezoidationEdge data);

    /*!
        * Check (recursively) whether there is any upward edge from the distance_to_boundary of the from of the \param edge
        *
        * \param strict Whether equidistant edges can count as a local maximum
        */
    bool canGoUp(bool strict = false) const;

    /*!
        * Check whether the edge goes from a lower to a higher distance_to_boundary.
        * Effectively deals with equidistant edges by looking beyond this edge.
        */
    bool isUpward() const;

    /*!
        * Calculate the traversed distance until we meet an upward edge.
        * Useful for calling on edges between equidistant points.
        *
        * If we can go up then the distance includes the length of the \param edge
        */
    std::optional<coord_t> distToGoUp() const;

    STHalfEdge* getNextUnconnected();
};

class STHalfEdgeNode : public HalfEdgeNode<SkeletalTrapezoidationJoint, SkeletalTrapezoidationEdge, STHalfEdgeNode, STHalfEdge>
{
    using edge_t = STHalfEdge;
    using node_t = STHalfEdgeNode;
public:
    STHalfEdgeNode(SkeletalTrapezoidationJoint data, Point p);

    bool isMultiIntersection();

    bool isCentral() const;

    /*!
        * Check whether this node has a locally maximal distance_to_boundary
        *
        * \param strict Whether equidistant edges can count as a local maximum
        */
    bool isLocalMaximum(bool strict = false) const;
};

class SkeletalTrapezoidationGraph: public HalfEdgeGraph<SkeletalTrapezoidationJoint, SkeletalTrapezoidationEdge, STHalfEdgeNode, STHalfEdge>
{
    using edge_t = STHalfEdge;
    using node_t = STHalfEdgeNode;
public:

    /*!
     * If an edge is too small, collapse it and its twin and fix the surrounding edges to ensure a consistent graph.
     * 
     * Don't collapse support edges, unless we can collapse the whole quad.
     * 
     * o-,
     * |  "-o
     * |    | > Don't collapse this edge only.
     * o    o
     */
    void collapseSmallEdges(coord_t snap_dist = 5);

    void makeRib(edge_t*& prev_edge, const Point &start_source_point, const Point &end_source_point);

    /*!
     * Insert a node into the graph and connect it to the input polygon using ribs
     * 
     * \return the last edge which replaced [edge], which points to the same [to] node
     */
    edge_t* insertNode(edge_t* edge, Point mid, coord_t mide_node_bead_count);

    /*!
     * Return the first and last edge of the edges replacing \p edge pointing to the same node
     */
    std::pair<edge_t*, edge_t*> insertRib(edge_t& edge, node_t* mid_node);

protected:
    Line getSource(const edge_t& edge) const;
};

}
#endif 
