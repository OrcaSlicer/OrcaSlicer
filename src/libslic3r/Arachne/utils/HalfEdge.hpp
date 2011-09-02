//Copyright (c) 2020 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef UTILS_HALF_EDGE_H
#define UTILS_HALF_EDGE_H

#include <forward_list>
#include <optional>

namespace Slic3r::Arachne
{

template<typename node_data_t, typename edge_data_t, typename derived_node_t, typename derived_edge_t>
class HalfEdgeNode;


template<typename node_data_t, typename edge_data_t, typename derived_node_t, typename derived_edge_t>
class HalfEdge
{
    using edge_t = derived_edge_t;
    using node_t = derived_node_t;
public:
    edge_data_t data;
    edge_t* twin = nullptr;
    edge_t* next = nullptr;
    edge_t* prev = nullptr;
    node_t* from = nullptr;
    node_t* to = nullptr;
    HalfEdge(edge_data_t data)
    : data(data)
    {}
    bool operator==(const edge_t& other)
    {
        return this == &other;
    }
};

} // namespace Slic3r::Arachne
#endif // UTILS_HALF_EDGE_H
