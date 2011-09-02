//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_HALF_EDGE_GRAPH_H
#define UTILS_HALF_EDGE_GRAPH_H


#include <list>
#include <cassert>



#include "HalfEdge.hpp"
#include "HalfEdgeNode.hpp"

namespace Slic3r::Arachne
{
template<class node_data_t, class edge_data_t, class derived_node_t, class derived_edge_t> // types of data contained in nodes and edges
class HalfEdgeGraph
{
public:
    using edge_t = derived_edge_t;
    using node_t = derived_node_t;
    using Edges = std::list<edge_t>;
    using Nodes = std::list<node_t>;
    Edges edges;
    Nodes nodes;
};

} // namespace Slic3r::Arachne
#endif // UTILS_HALF_EDGE_GRAPH_H
