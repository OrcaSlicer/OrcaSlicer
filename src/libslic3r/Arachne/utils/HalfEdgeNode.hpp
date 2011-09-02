//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_HALF_EDGE_NODE_H
#define UTILS_HALF_EDGE_NODE_H

#include <list>

#include "../../Point.hpp"

namespace Slic3r::Arachne
{

template<typename node_data_t, typename edge_data_t, typename derived_node_t, typename derived_edge_t>
class HalfEdge;

template<typename node_data_t, typename edge_data_t, typename derived_node_t, typename derived_edge_t>
class HalfEdgeNode
{
    using edge_t = derived_edge_t;
    using node_t = derived_node_t;
public:
    node_data_t data;
    Point p;
    edge_t* incident_edge = nullptr;
    HalfEdgeNode(node_data_t data, Point p)
    : data(data)
    , p(p)
    {}

    bool operator==(const node_t& other)
    {
        return this == &other;
    }
};

} // namespace Slic3r::Arachne
#endif // UTILS_HALF_EDGE_NODE_H
