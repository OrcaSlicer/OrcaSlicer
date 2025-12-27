/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef _CONSTRAINED_DELAUNAY_TRIANGULATION_H_
#define _CONSTRAINED_DELAUNAY_TRIANGULATION_H_

#include "mcut/internal/cdt/triangulate.h"
#include "mcut/internal/cdt/utils.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <stack>
#include <vector>

/// Namespace containing triangulation functionality
namespace cdt {

/** @defgroup API Public API
 *  Contains API for constrained and conforming Delaunay triangulations
 */
/// @{

/**
 * Type used for storing layer depths for triangles
 * @note layer_depth_t should support 60K+ layers, which could be to much or
 * too little for some use cases. Feel free to re-define this typedef.
 */
typedef unsigned short layer_depth_t;
typedef layer_depth_t boundary_overlap_count_t;

/** @defgroup helpers Helpers
 *  Helpers for working with cdt::triangulator_t.
 */
/// @{

/**
 * Calculate triangles adjacent to vertices (triangles by vertex index)
 * @param triangles triangulation
 * @param verticesSize total number of vertices to pre-allocate the output
 * @return triangles by vertex index (array of size V, where V is the number of vertices; each element is a list of triangles incident to corresponding vertex).
 */
inline std::vector<std::vector<std::uint32_t>> get_vertex_to_triangles_map(
    const std::vector<triangle_t>& triangles,
    const std::uint32_t verticesSize)
{
    std::vector<std::vector<std::uint32_t>> map(verticesSize);

    for (std::uint32_t i = 0; i < (std::uint32_t)triangles.size(); ++i) {

        const std::uint32_t triangle_index = i;
        const triangle_t& triangle = triangles[triangle_index];
        const std::array<std::uint32_t, 3>& vertices = triangle.vertices;

        for (std::array<std::uint32_t, 3>::const_iterator j = vertices.begin(); j != vertices.end(); ++j) {
            const std::uint32_t vertex_index = *j;
            map[vertex_index].push_back(i);
        }
    }

    return map;
}

/**
 * Information about removed duplicated vertices.
 *
 * Contains mapping information and removed duplicates indices.
 * @note vertices {0,1,2,3,4} where 0 and 3 are the same will produce mapping
 *       {0,1,2,0,3} (to new vertices {0,1,2,3}) and duplicates {3}
 */
struct duplicates_info_t {
    std::vector<std::size_t> mapping; ///< vertex index mapping
    std::vector<std::size_t> duplicates; ///< duplicates' indices
};

/*!
 * Remove elements in the range [first; last) with indices from the sorted
 * unique range [ii_first, ii_last)
 */
template <class ForwardIt, class SortUniqIndsFwdIt>
inline ForwardIt remove_at(
    ForwardIt first,
    ForwardIt last,
    SortUniqIndsFwdIt ii_first,
    SortUniqIndsFwdIt ii_last)
{
    if (ii_first == ii_last) // no indices-to-remove are given
        return last;

    typedef typename std::iterator_traits<ForwardIt>::difference_type diff_t;
    typedef typename std::iterator_traits<SortUniqIndsFwdIt>::value_type ind_t;

    ForwardIt destination = first + static_cast<diff_t>(*ii_first);

    while (ii_first != ii_last) {

        // advance to an index after a chunk of elements-to-keep
        for (ind_t cur = *ii_first++; ii_first != ii_last; ++ii_first) {
            const ind_t nxt = *ii_first;
            if (nxt - cur > 1)
                break;
            cur = nxt;
        }

        // move the chunk of elements-to-keep to new destination
        const ForwardIt source_first = first + static_cast<diff_t>(*(ii_first - 1)) + 1;
        const ForwardIt source_last = ii_first != ii_last ? first + static_cast<diff_t>(*ii_first) : last;

        std::move(source_first, source_last, destination);

        destination += source_last - source_first;
    }
    return destination;
}

/**
 * Find duplicates in given custom point-type range
 * @note duplicates are points with exactly same X and Y coordinates
 * @tparam TVertexIter iterator that dereferences to custom point type
 * @tparam TGetVertexCoordX function object getting x coordinate from vertex.
 * Getter signature: const TVertexIter::value_type& -> T
 * @tparam TGetVertexCoordY function object getting y coordinate from vertex.
 * Getter signature: const TVertexIter::value_type& -> T
 * @param first beginning of the range of vertices
 * @param last end of the range of vertices
 * @param get_x_coord getter of X-coordinate
 * @param get_y_coord getter of Y-coordinate
 * @returns information about vertex duplicates
 */
template <
    typename T,
    typename TVertexIter,
    typename TGetVertexCoordX,
    typename TGetVertexCoordY>
duplicates_info_t find_duplicates(
    TVertexIter first,
    TVertexIter last,
    TGetVertexCoordX get_x_coord,
    TGetVertexCoordY get_y_coord)
{
    std::unordered_map<vec2_<T>, std::size_t> uniqueVerts; // position to index map
    const std::size_t verticesSize = std::distance(first, last);

    duplicates_info_t di = {
        std::vector<std::size_t>(verticesSize),
        std::vector<std::size_t>()
    };

    for (std::size_t iIn = 0, iOut = iIn; iIn < verticesSize; ++iIn, ++first) {
        typename std::unordered_map<vec2_<T>, std::size_t>::const_iterator it;
        bool isUnique;

        // check if the coordinates match [exactly] (in the bitwise sense)
        std::tie(it, isUnique) = uniqueVerts.insert(
            std::make_pair(
                vec2_<T>::make(get_x_coord(*first), get_y_coord(*first)),
                iOut));

        if (isUnique) {
            di.mapping[iIn] = iOut++;
            continue;
        }

        di.mapping[iIn] = it->second; // found a duplicate
        di.duplicates.push_back(iIn);
    }

    return di;
}

/**
 * Remove duplicates in-place from vector of custom points
 * @tparam TVertex vertex type
 * @tparam TAllocator allocator used by input vector of vertices
 * @param vertices vertices to remove duplicates from
 * @param duplicates information about duplicates
 */
template <typename TVertex, typename TAllocator>
void remove_duplicates(
    std::vector<TVertex, TAllocator>& vertices,
    const std::vector<std::size_t>& duplicates)
{
    vertices.erase(
        remove_at(
            vertices.begin(),
            vertices.end(),
            duplicates.begin(),
            duplicates.end()),
        vertices.end());
}

/**
 * Remove duplicated points in-place
 *
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @param[in, out] vertices collection of vertices to remove duplicates from
 * @returns information about duplicated vertices that were removed.
 */
template <typename T>
duplicates_info_t remove_duplicates(std::vector<vec2_<T>>& vertices)
{
    const duplicates_info_t di = find_duplicates<T>(
        vertices.begin(),
        vertices.end(),
        get_x_coord_vec2d<T>,
        get_y_coord_vec2d<T>);

    remove_duplicates(vertices, di.duplicates);

    return di;
}

/**
 * Remap vertex indices in edges (in-place) using given vertex-index mapping.
 * @tparam TEdgeIter iterator that dereferences to custom edge type
 * @tparam TGetEdgeVertexStart function object getting start vertex index
 * from an edge.
 * Getter signature: const TEdgeIter::value_type& -> std::uint32_t
 * @tparam TGetEdgeVertexEnd function object getting end vertex index from
 * an edge. Getter signature: const TEdgeIter::value_type& -> std::uint32_t
 * @tparam TMakeEdgeFromStartAndEnd function object that makes new edge from
 * start and end vertices
 * @param first beginning of the range of edges
 * @param last end of the range of edges
 * @param mapping vertex-index mapping
 * @param getStart getter of edge start vertex index
 * @param getEnd getter of edge end vertex index
 * @param makeEdge factory for making edge from vetices
 */

template <
    typename TEdgeIter,
    typename TGetEdgeVertexStart,
    typename TGetEdgeVertexEnd,
    typename TMakeEdgeFromStartAndEnd>
void remap_edges(
    TEdgeIter first,
    const TEdgeIter last,
    const std::vector<std::size_t>& mapping,
    TGetEdgeVertexStart getStart,
    TGetEdgeVertexEnd getEnd,
    TMakeEdgeFromStartAndEnd makeEdge)
{
    for (; first != last; ++first) {
        *first = makeEdge(
            static_cast<std::uint32_t>(mapping[getStart(*first)]),
            static_cast<std::uint32_t>(mapping[getEnd(*first)]));
    }
}

/**
 * Remap vertex indices in edges (in-place) using given vertex-index mapping.
 *
 * @note Mapping can be a result of remove_duplicates function
 * @param[in,out] edges collection of edges to remap
 * @param mapping vertex-index mapping
 */
inline void
remap_edges(std::vector<edge_t>& edges, const std::vector<std::size_t>& mapping)
{
    remap_edges(
        edges.begin(),
        edges.end(),
        mapping,
        edge_get_v1,
        edge_get_v2,
        edge_make);
}

/**
 * Find point duplicates, remove them from vector (in-place) and remap edges
 * (in-place)
 * @note Same as a chained call of cdt::find_duplicates, cdt::remove_duplicates,
 * and cdt::remap_edges
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @tparam TVertex type of vertex
 * @tparam TGetVertexCoordX function object getting x coordinate from vertex.
 * Getter signature: const TVertexIter::value_type& -> T
 * @tparam TGetVertexCoordY function object getting y coordinate from vertex.
 * Getter signature: const TVertexIter::value_type& -> T
 * @tparam TEdgeIter iterator that dereferences to custom edge type
 * @tparam TGetEdgeVertexStart function object getting start vertex index
 * from an edge.
 * Getter signature: const TEdgeIter::value_type& -> std::uint32_t
 * @tparam TGetEdgeVertexEnd function object getting end vertex index from
 * an edge. Getter signature: const TEdgeIter::value_type& -> std::uint32_t
 * @tparam TMakeEdgeFromStartAndEnd function object that makes new edge from
 * start and end vertices
 * @param[in, out] vertices vertices to remove duplicates from
 * @param[in, out] edges collection of edges connecting vertices
 * @param get_x_coord getter of X-coordinate
 * @param get_y_coord getter of Y-coordinate
 * @param edgesFirst beginning of the range of edges
 * @param edgesLast end of the range of edges
 * @param getStart getter of edge start vertex index
 * @param getEnd getter of edge end vertex index
 * @param makeEdge factory for making edge from vetices
 * @returns information about vertex duplicates
 */

template <
    typename T,
    typename TVertex,
    typename TGetVertexCoordX,
    typename TGetVertexCoordY,
    typename TVertexAllocator,
    typename TEdgeIter,
    typename TGetEdgeVertexStart,
    typename TGetEdgeVertexEnd,
    typename TMakeEdgeFromStartAndEnd>
duplicates_info_t remove_duplicates_and_remap_edges(
    std::vector<TVertex, TVertexAllocator>& vertices,
    TGetVertexCoordX get_x_coord,
    TGetVertexCoordY get_y_coord,
    const TEdgeIter edgesFirst,
    const TEdgeIter edgesLast,
    TGetEdgeVertexStart getStart,
    TGetEdgeVertexEnd getEnd,
    TMakeEdgeFromStartAndEnd makeEdge)
{
    const duplicates_info_t di = find_duplicates<T>(vertices.begin(), vertices.end(), get_x_coord, get_y_coord);

    remove_duplicates(vertices, di.duplicates);
    remap_edges(edgesFirst, edgesLast, di.mapping, getStart, getEnd, makeEdge);

    return di;
}

/**
 * Same as a chained call of cdt::remove_duplicates + cdt::remap_edges
 *
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @param[in, out] vertices collection of vertices to remove duplicates from
 * @param[in,out] edges collection of edges to remap
 */
template <typename T>
duplicates_info_t remove_duplicates_and_remap_edges(
    std::vector<vec2_<T>>& vertices,
    std::vector<edge_t>& edges)
{
    return remove_duplicates_and_remap_edges<T>(
        vertices,
        get_x_coord_vec2d<T>,
        get_y_coord_vec2d<T>,
        edges.begin(),
        edges.end(),
        edge_get_v1,
        edge_get_v2,
        edge_make);
}

/**
 * Extract all edges of triangles
 *
 * @param triangles triangles used to extract edges
 * @return an unordered set of all edges of triangulation
 */
inline std::unordered_set<edge_t>
extract_edges_from_triangles(const std::vector<triangle_t>& triangles)
{
    std::unordered_set<edge_t> edges;

    for (std::vector<triangle_t>::const_iterator t = triangles.begin(); t != triangles.end(); ++t) {
        edges.insert(edge_t(std::uint32_t(t->vertices[0]), std::uint32_t(t->vertices[1])));
        edges.insert(edge_t(std::uint32_t(t->vertices[1]), std::uint32_t(t->vertices[2])));
        edges.insert(edge_t(std::uint32_t(t->vertices[2]), std::uint32_t(t->vertices[0])));
    }
    return edges;
}

/*!
 * Converts piece->original_edges mapping to original_edge->pieces
 * @param pieceToOriginals maps pieces to original edges
 * @return mapping of original edges to pieces
 */
inline std::unordered_map<edge_t, std::vector<edge_t>>
edge_to_pieces_mapping(const std::unordered_map<edge_t, std::vector<edge_t>>& pieceToOriginals)
{
    std::unordered_map<edge_t, std::vector<edge_t>> originalToPieces;
    typedef std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator Cit;
    for (Cit ptoIt = pieceToOriginals.begin(); ptoIt != pieceToOriginals.end();
         ++ptoIt) {
        const edge_t piece = ptoIt->first;
        const std::vector<edge_t>& originals = ptoIt->second;
        for (std::vector<edge_t>::const_iterator origIt = originals.begin();
             origIt != originals.end();
             ++origIt) {
            originalToPieces[*origIt].push_back(piece);
        }
    }
    return originalToPieces;
}

/*!
 * Convert edge-to-pieces mapping into edge-to-split-vertices mapping
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @param edgeToPieces edge-to-pieces mapping
 * @param vertices vertex buffer
 * @return mapping of edge-to-split-points.
 * Split points are sorted from edge's start (v1) to end (v2)
 */

template <typename T>
std::unordered_map<edge_t, std::vector<std::uint32_t>> get_edge_to_split_vertices_map(
    const std::unordered_map<edge_t, std::vector<edge_t>>& edgeToPieces,
    const std::vector<vec2_<T>>& vertices)
{
    typedef std::pair<std::uint32_t, T> VertCoordPair;

    struct ComparePred {
        bool operator()(const VertCoordPair& a, const VertCoordPair& b) const
        {
            return a.second < b.second;
        }
    } comparePred;

    std::unordered_map<edge_t, std::vector<std::uint32_t>> edgeToSplitVerts;
    
    for (std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator it = edgeToPieces.begin();
         it != edgeToPieces.end();
         ++it) {

        const edge_t& e = it->first;
        const T dX = vertices[e.v2()].x() - vertices[e.v1()].x();
        const T dY = vertices[e.v2()].y() - vertices[e.v1()].y();
        const bool isX = std::abs(dX) >= std::abs(dY); // X-coord longer
        const bool isAscending = isX ? dX >= 0 : dY >= 0; // Longer coordinate ascends
        const std::vector<edge_t>& pieces = it->second;

        std::vector<VertCoordPair> splitVerts;
        // size is:  2[ends] + (pieces - 1)[split vertices] = pieces + 1
        splitVerts.reserve(pieces.size() + 1);

        for (std::vector<edge_t>::const_iterator it = pieces.begin(); it != pieces.end(); ++it) {

            const std::array<std::uint32_t, 2> vv = { it->v1(), it->v2() };

            for (std::array<std::uint32_t, 2>::const_iterator v = vv.begin(); v != vv.end(); ++v) {
                const T c = isX ? vertices[*v].x() : vertices[*v].y();
                splitVerts.push_back(std::make_pair(*v, isAscending ? c : -c));
            }
        }

        // sort by longest coordinate
        std::sort(splitVerts.begin(), splitVerts.end(), comparePred);

        // remove duplicates
        splitVerts.erase(
            std::unique(splitVerts.begin(), splitVerts.end()),
            splitVerts.end());

        MCUT_ASSERT(splitVerts.size() > 2); // 2 end points with split vertices

        std::pair<edge_t, std::vector<std::uint32_t>> val = std::make_pair(e, std::vector<std::uint32_t>());

        val.second.reserve(splitVerts.size());

        for (typename std::vector<VertCoordPair>::const_iterator it = splitVerts.begin() + 1;
             it != splitVerts.end() - 1;
             ++it) {
            val.second.push_back(it->first);
        }

        edgeToSplitVerts.insert(val);
    }
    return edgeToSplitVerts;
}

/// @}

/// @}

} // namespace cdt

//*****************************************************************************
// Implementations of template functionlity
//*****************************************************************************
// hash for vec2_<T>
namespace std {

template <typename T>
struct hash<vec2_<T>> {
    size_t operator()(const vec2_<T>& xy) const
    {
        return std::hash<T>()(xy.x()) ^ std::hash<T>()(xy.y());
    }
};

} // namespace std

namespace cdt {

//-----
// API
//-----

/**
 * Verify that triangulation topology is consistent.
 *
 * Checks:
 *  - for each vertex adjacent triangles contain the vertex
 *  - each triangle's neighbor in turn has triangle as its neighbor
 *  - each of triangle's vertices has triangle as adjacent
 *
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @tparam TNearPointLocator class providing locating near point for efficiently
 */
template <typename T, typename TNearPointLocator>
inline bool check_topology(const cdt::triangulator_t<T, TNearPointLocator>& cdt)
{
    // Check if vertices' adjacent triangles contain vertex
    const std::vector<std::vector<std::uint32_t>> vertTris = cdt.is_finalized()
        ? get_vertex_to_triangles_map(
            cdt.triangles, static_cast<std::uint32_t>(cdt.vertices.size()))
        : cdt.vertTris;
    for (std::uint32_t iV(0); iV < std::uint32_t(cdt.vertices.size()); ++iV) {
        const std::vector<std::uint32_t>& vTris = vertTris[iV];
        typedef std::vector<std::uint32_t>::const_iterator TriIndCit;
        for (TriIndCit it = vTris.begin(); it != vTris.end(); ++it) {
            const std::array<std::uint32_t, 3>& vv = cdt.triangles[*it].vertices;
            if (std::find(vv.begin(), vv.end(), iV) == vv.end())
                return false;
        }
    }
    // Check if triangle neighbor links are fine
    for (std::uint32_t iT(0); iT < std::uint32_t(cdt.triangles.size()); ++iT) {
        const triangle_t& t = cdt.triangles[iT];
        typedef std::array<std::uint32_t, 3>::const_iterator NCit;
        for (NCit it = t.neighbors.begin(); it != t.neighbors.end(); ++it) {
            if (*it == null_neighbour)
                continue;
            const std::array<std::uint32_t, 3>& nn = cdt.triangles[*it].neighbors;
            if (std::find(nn.begin(), nn.end(), iT) == nn.end())
                return false;
        }
    }
    // Check if triangle's vertices have triangle as adjacent
    for (std::uint32_t iT(0); iT < std::uint32_t(cdt.triangles.size()); ++iT) {
        const triangle_t& t = cdt.triangles[iT];
        typedef std::array<std::uint32_t, 3>::const_iterator VCit;
        for (VCit it = t.vertices.begin(); it != t.vertices.end(); ++it) {
            const std::vector<std::uint32_t>& tt = vertTris[*it];
            if (std::find(tt.begin(), tt.end(), iT) == tt.end())
                return false;
        }
    }
    return true;
}

} // namespace cdt

#endif // #ifndef _CONSTRAINED_DELAUNAY_TRIANGULATION_H_
