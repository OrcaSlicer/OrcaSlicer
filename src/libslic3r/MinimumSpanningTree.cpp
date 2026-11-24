#include "MinimumSpanningTree.hpp"

#include <cstddef>
#include <iterator>
#include <algorithm>
#include "Point.hpp"
#include <vector>
#include <unordered_map>
#include <utility>
#include "libslic3r.h"

namespace Slic3r
{

#define unscale_(val) ((val) * SCALING_FACTOR)

inline double dot_with_unscale(const Point a, const Point b)
{
    return unscale_(a(0)) * unscale_(b(0)) + unscale_(a(1)) * unscale_(b(1));
}

inline double vsize2_with_unscale(const Point pt)
{
    return dot_with_unscale(pt, pt);
}

MinimumSpanningTree::MinimumSpanningTree(std::vector<Point> vertices) : adjacency_graph(prim(vertices))
{
    //Just copy over the fields.
}

auto MinimumSpanningTree::prim(std::vector<Point> vertices) const -> AdjacencyGraph_t
{
    AdjacencyGraph_t result;
    if (vertices.empty())
    {
        return result; //No vertices, so we can't create edges either.
    }
    // If there's only one vertex, we can't go creating any edges so just add the point to the adjacency list with no
    // edges
    if (vertices.size() == 1)
    {
        // unordered_map::operator[]() will construct an empty vector in place for us when we try and access an element
        // that doesnt exist
        result[*vertices.begin()];
        return result;
    }
    result.reserve(vertices.size());

    // For each vertex not yet in the tree: its shortest distance to the tree and the tree vertex it goes to.
    // Every step visits all of them, so they are kept in plain arrays indexed by vertex.
    std::vector<coordf_t> smallest_distance(vertices.size());
    std::vector<size_t>   smallest_distance_to(vertices.size(), 0);
    std::vector<size_t>   candidates;
    candidates.reserve(vertices.size() - 1);
    for (size_t vertex_index = 1; vertex_index < vertices.size(); vertex_index++)
    {
        smallest_distance[vertex_index] = vsize2_with_unscale(vertices[vertex_index] - vertices[0]);
        candidates.push_back(vertex_index);
    }

    while (!candidates.empty()) //All of the vertices need to be in the tree at the end.
    {
        //Choose the closest vertex to connect to that is not yet in the tree.
        //This search is O(V) right now, which can be made down to O(log(V)). This reduces the overall time complexity from O(V*V) to O(V*log(E)).
        //However that requires an implementation of a heap that supports the decreaseKey operation, which is not in the std library.
        // Break equal-distance ties on coordinates, so the choice does not depend on the order of the candidates.
        size_t best = 0;
        for (size_t candidate = 1; candidate < candidates.size(); candidate++)
        {
            const size_t a = candidates[candidate];
            const size_t b = candidates[best];
            if (smallest_distance[a] != smallest_distance[b] ? smallest_distance[a] < smallest_distance[b] : vertices[a] < vertices[b])
                best = candidate;
        }
        const size_t closest = candidates[best];
        candidates[best] = candidates.back(); //Remove it so we don't check for this point again.
        candidates.pop_back();

        //Add this point to the graph.
        const Point& closest_point = vertices[closest];
        const Point other_end = vertices[smallest_distance_to[closest]];
        result[closest_point].push_back({closest_point, other_end});
        result[other_end].push_back({other_end, closest_point});

        //Update the distances of all points that are not in the graph.
        for (const size_t candidate : candidates)
        {
            const coordf_t new_distance = vsize2_with_unscale(closest_point - vertices[candidate]);
            if (new_distance < smallest_distance[candidate]) //New point is closer.
            {
                smallest_distance[candidate] = new_distance;
                smallest_distance_to[candidate] = closest;
            }
        }
    }

    return result;
}

std::vector<Point> MinimumSpanningTree::adjacent_nodes(Point node) const
{
    std::vector<Point> result;
    AdjacencyGraph_t::const_iterator adjacency_entry = adjacency_graph.find(node);
    if (adjacency_entry != adjacency_graph.end())
    {
        const auto& edges = adjacency_entry->second;
        std::transform(edges.begin(), edges.end(), std::back_inserter(result),
                       [&node](const Edge& e) { return (e.start == node) ? e.end : e.start; });
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::leaves() const
{
    std::vector<Point> result;
    for (std::pair<Point, std::vector<Edge>> node : adjacency_graph)
    {
        if (node.second.size() <= 1) //Leaves are nodes that have only one adjacent edge, or just the one node if the tree contains one node.
        {
            result.push_back(node.first);
        }
    }
    return result;
}

std::vector<Point> MinimumSpanningTree::vertices() const
{
    std::vector<Point> result;
    using MapValue = std::pair<Point, std::vector<Edge>>;
    std::transform(adjacency_graph.begin(), adjacency_graph.end(), std::back_inserter(result),
                   [](const MapValue& node) { return node.first; });
    return result;
}

}
