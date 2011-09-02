/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CDT_vW1vZ0lO8rS4gY4uI4fB
#define CDT_vW1vZ0lO8rS4gY4uI4fB

#include "mcut/internal/cdt/kdtree.h"
#include "mcut/internal/cdt/utils.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <stack>
#include <stdexcept>
#include <utility>
#include <vector>

/// Namespace containing triangulation functionality
namespace cdt {

/// @addtogroup API
/// @{

/**
 * Enum of strategies specifying order in which a range of vertices is inserted
 * @note vertex_insertion_order_t::RANDOM will only randomize order of
 * inserting in triangulation, vertex indices will be preserved as they were
 * specified in the final triangulation
 */
struct vertex_insertion_order_t {
    /**
     * The Enum itself
     * @note needed to pre c++11 compilers that don't support 'class enum'
     */
    enum Enum {
        RANDOM, ///< vertices will be inserted in random order
        AS_GIVEN, ///< vertices will be inserted in the same order as provided
    };
};

/// Enum of what type of geometry used to embed triangulation into
struct super_geometry_type_t {
    /**
     * The Enum itself
     * @note needed to pre c++11 compilers that don't support 'class enum'
     */
    enum Enum {
        SUPER_TRIANGLE, ///< conventional super-triangle
        CUSTOM, ///< user-specified custom geometry (e.g., grid)
    };
};

/**
 * Enum of strategies for treating intersecting constraint edges
 */
struct action_on_intersecting_constraint_edges_t {
    /**
     * The Enum itself
     * @note needed to pre c++11 compilers that don't support 'class enum'
     */
    enum Enum {
        IGNORE, ///< constraint edge intersections are not checked
        RESOLVE, ///< constraint edge intersections are resolved
    };
};

/**
 * Type used for storing layer depths for triangles
 * @note layer_depth_t should support 60K+ layers, which could be to much or
 * too little for some use cases. Feel free to re-define this typedef.
 */
typedef unsigned short layer_depth_t;
typedef layer_depth_t boundary_overlap_count_t;

namespace detail {

    /// Needed for c++03 compatibility (no uniform initialization available)
    template <typename T>
    std::array<T, 3> arr3(const T& v0, const T& v1, const T& v2)
    {
        const std::array<T, 3> out = { v0, v1, v2 };
        return out;
    }

    namespace defaults {

        const std::size_t nTargetVerts = 0;
        const super_geometry_type_t::Enum superGeomType = super_geometry_type_t::SUPER_TRIANGLE;
        const vertex_insertion_order_t::Enum vertexInsertionOrder = vertex_insertion_order_t::RANDOM;
        const action_on_intersecting_constraint_edges_t::Enum intersectingEdgesStrategy = action_on_intersecting_constraint_edges_t::IGNORE;
        const float minDistToConstraintEdge(0);

    } // namespace defaults

    // add element to 'to' if not already in 'to'
    template <typename T, typename Allocator1>
    void insert_unique(std::vector<T, Allocator1>& to, const T& elem)
    {
        if (std::find(to.begin(), to.end(), elem) == to.end()) {
            to.push_back(elem);
        }
    }

    // add elements of 'from' that are not present in 'to' to 'to'
    template <typename T, typename Allocator1, typename Allocator2>
    void insert_unique(
        std::vector<T, Allocator1>& to,
        const std::vector<T, Allocator2>& from)
    {
        typedef typename std::vector<T, Allocator2>::const_iterator Cit;
        to.reserve(to.size() + from.size());
        for (Cit cit = from.begin(); cit != from.end(); ++cit) {
            insert_unique(to, *cit);
        }
    }

} // namespace detail

/**
 * @defgroup triangulator_t triangulator_t Class
 * Class performing triangulations.
 */
/// @{

/**
 * Data structure representing a 2D constrained Delaunay triangulation
 *
 * @tparam T type of vertex coordinates (e.g., float, double)
 * @tparam TNearPointLocator class providing locating near point for efficiently
 * inserting new points. Provides methods: 'add_point(vPos, iV)' and
 * 'nearPoint(vPos) -> iV'
 */
template <typename T, typename TNearPointLocator = locator_kdtree_t<T>>
class triangulator_t {
public:
    // typedef std::vector<vec2_<T>> vec2_vector_t; ///< Vertices vector
    std::vector<vec2_<T>> vertices; ///< triangulation's vertices
    std::vector<triangle_t> triangles; ///< triangulation's triangles
    std::unordered_set<edge_t> fixedEdges; ///< triangulation's constraints (fixed edges)
    /**
     * triangles adjacent to each vertex
     * @note will be reset to empty when super-triangle is removed and
     * triangulation is finalized. To re-calculate adjacent triangles use
     * cdt::get_vertex_to_triangles_map helper
     */
    std::vector<std::vector<std::uint32_t>> vertTris;

    /** Stores count of overlapping boundaries for a fixed edge. If no entry is
     * present for an edge: no boundaries overlap.
     * @note map only has entries for fixed for edges that represent overlapping
     * boundaries
     * @note needed for handling depth calculations and hole-removel in case of
     * overlapping boundaries
     */
    std::unordered_map<edge_t, boundary_overlap_count_t> overlapCount;

    /** Stores list of original edges represented by a given fixed edge
     * @note map only has entries for edges where multiple original fixed edges
     * overlap or where a fixed edge is a part of original edge created by
     * conforming Delaunay triangulation vertex insertion
     */
    std::unordered_map<edge_t, std::vector<edge_t>> pieceToOriginals;

    /*____ API _____*/
    /// Default constructor
    triangulator_t();
    /**
     * Constructor
     * @param vertexInsertionOrder strategy used for ordering vertex insertions
     */
    triangulator_t(vertex_insertion_order_t::Enum vertexInsertionOrder);
    /**
     * Constructor
     * @param vertexInsertionOrder strategy used for ordering vertex insertions
     * @param intersectingEdgesStrategy strategy for treating intersecting
     * constraint edges
     * @param minDistToConstraintEdge distance within which point is considered
     * to be lying on a constraint edge. Used when adding constraints to the
     * triangulation.
     */
    triangulator_t(
        vertex_insertion_order_t::Enum vertexInsertionOrder,
        action_on_intersecting_constraint_edges_t::Enum intersectingEdgesStrategy,
        T minDistToConstraintEdge);
    /**
     * Constructor
     * @param vertexInsertionOrder strategy used for ordering vertex insertions
     * @param nearPtLocator class providing locating near point for efficiently
     * inserting new points
     * @param intersectingEdgesStrategy strategy for treating intersecting
     * constraint edges
     * @param minDistToConstraintEdge distance within which point is considered
     * to be lying on a constraint edge. Used when adding constraints to the
     * triangulation.
     */
    triangulator_t(
        vertex_insertion_order_t::Enum vertexInsertionOrder,
        const TNearPointLocator& nearPtLocator,
        action_on_intersecting_constraint_edges_t::Enum intersectingEdgesStrategy,
        T minDistToConstraintEdge);
    /**
     * Insert custom point-types specified by iterator range and X/Y-getters
     * @tparam TVertexIter iterator that dereferences to custom point type
     * @tparam TGetVertexCoordX function object getting x coordinate from
     * vertex. Getter signature: const TVertexIter::value_type& -> T
     * @tparam TGetVertexCoordY function object getting y coordinate from
     * vertex. Getter signature: const TVertexIter::value_type& -> T
     * @param first beginning of the range of vertices to add
     * @param last end of the range of vertices to add
     * @param get_x_coord getter of X-coordinate
     * @param get_y_coord getter of Y-coordinate
     */
    template <
        typename TVertexIter,
        typename TGetVertexCoordX,
        typename TGetVertexCoordY>
    void insert_vertices(
        TVertexIter first,
        TVertexIter last,
        TGetVertexCoordX get_x_coord,
        TGetVertexCoordY get_y_coord);
    /**
     * Insert vertices into triangulation
     * @param vertices vector of vertices to insert
     */
    void insert_vertices(const std::vector<vec2_<T>>& vertices);
    /**
     * Insert constraints (custom-type fixed edges) into triangulation
     * @note Each fixed edge is inserted by deleting the triangles it crosses,
     * followed by the triangulation of the polygons on each side of the edge.
     * <b> No new vertices are inserted.</b>
     * @note If some edge appears more than once in the input this means that
     * multiple boundaries overlap at the edge and impacts how hole detection
     * algorithm of triangulator_t::erase_outer_triangles_and_holes works.
     * <b>Make sure there are no erroneous duplicates.</b>
     * @tparam TEdgeIter iterator that dereferences to custom edge type
     * @tparam TGetEdgeVertexStart function object getting start vertex index
     * from an edge.
     * Getter signature: const TEdgeIter::value_type& -> std::uint32_t
     * @tparam TGetEdgeVertexEnd function object getting end vertex index from
     * an edge. Getter signature: const TEdgeIter::value_type& -> std::uint32_t
     * @param first beginning of the range of edges to add
     * @param last end of the range of edges to add
     * @param getStart getter of edge start vertex index
     * @param getEnd getter of edge end vertex index
     */
    template <
        typename TEdgeIter,
        typename TGetEdgeVertexStart,
        typename TGetEdgeVertexEnd>
    void insert_edges(
        TEdgeIter first,
        TEdgeIter last,
        TGetEdgeVertexStart getStart,
        TGetEdgeVertexEnd getEnd);
    /**
     * Insert constraint edges into triangulation
     * @note Each fixed edge is inserted by deleting the triangles it crosses,
     * followed by the triangulation of the polygons on each side of the edge.
     * <b> No new vertices are inserted.</b>
     * @note If some edge appears more than once in the input this means that
     * multiple boundaries overlap at the edge and impacts how hole detection
     * algorithm of triangulator_t::erase_outer_triangles_and_holes works.
     * <b>Make sure there are no erroneous duplicates.</b>
     * @tparam edges constraint edges
     */
    void insert_edges(const std::vector<edge_t>& edges);

    /*!
     * Returns:
     *  - intersected triangle index
     *  - index of point on the left of the line
     *  - index of point on the right of the line
     * If left point is right on the line: no triangle is intersected:
     *  - triangle index is no-neighbor (invalid)
     *  - index of point on the line
     *  - index of point on the right of the line
     */
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
    get_intersected_triangle(
        const std::uint32_t iA,
        const std::vector<std::uint32_t>& candidates,
        const vec2_<T>& a,
        const vec2_<T>& b,
        const T orientationTolerance) const
    {
        typedef std::vector<std::uint32_t>::const_iterator TriIndCit;
        for (TriIndCit it = candidates.begin(); it != candidates.end(); ++it) {
            const std::uint32_t iT = *it;
            const triangle_t t = triangles[iT];
            const std::uint32_t i = get_vertex_index(t, iA);
            const std::uint32_t iP2 = t.vertices[ccw(i)];
            const T orientP2 = orient2d(vertices[iP2], a, b);
            const point_to_line_location_t::Enum locP2 = classify_orientation(orientP2);

            if (locP2 == point_to_line_location_t::RIGHT_SIDE) {
                const std::uint32_t iP1 = t.vertices[cw(i)];
                const T orientP1 = orient2d(vertices[iP1], a, b);
                const point_to_line_location_t::Enum locP1 = classify_orientation(orientP1);
                if (locP1 == point_to_line_location_t::COLLINEAR) {
                    return std::make_tuple(null_neighbour, iP1, iP1);
                }
                if (locP1 == point_to_line_location_t::LEFT_SIDE) {
                    if (orientationTolerance) {
                        T closestOrient;
                        std::uint32_t iClosestP;
                        if (std::abs(orientP1) <= std::abs(orientP2)) {
                            closestOrient = orientP1;
                            iClosestP = iP1;
                        } else {
                            closestOrient = orientP2;
                            iClosestP = iP2;
                        }
                        if (classify_orientation(
                                closestOrient, orientationTolerance)
                            == point_to_line_location_t::COLLINEAR) {
                            return std::make_tuple(null_neighbour, iClosestP, iClosestP);
                        }
                    }
                    return std::make_tuple(iT, iP1, iP2);
                }
            }
        }
        throw std::runtime_error("Could not find vertex triangle intersected by "
                                 "edge. Note: can be caused by duplicate points.");
    }

    /// Returns indices of four resulting triangles
    /* Inserting a point on the edge between two triangles
     *    T1 (top)        v1
     *                   /|\
     *              n1 /  |  \ n4
     *               /    |    \
     *             /  T1' | Tnew1\
     *           v2-------v-------v4
     *             \ Tnew2| T2'  /
     *               \    |    /
     *              n2 \  |  / n3
     *                   \|/
     *   T2 (bottom)      v3
     */
    std::stack<std::uint32_t> insert_point_on_edge(
        const std::uint32_t v,
        const std::uint32_t iT1,
        const std::uint32_t iT2)
    {
        const std::uint32_t iTnew1 = add_triangle();
        const std::uint32_t iTnew2 = add_triangle();

        triangle_t& t1 = triangles[iT1];
        triangle_t& t2 = triangles[iT2];
        std::uint32_t i = get_opposite_vertex_index(t1, iT2);
        const std::uint32_t v1 = t1.vertices[i];
        const std::uint32_t v2 = t1.vertices[ccw(i)];
        const std::uint32_t n1 = t1.neighbors[i];
        const std::uint32_t n4 = t1.neighbors[cw(i)];
        i = get_opposite_vertex_index(t2, iT1);
        const std::uint32_t v3 = t2.vertices[i];
        const std::uint32_t v4 = t2.vertices[ccw(i)];
        const std::uint32_t n3 = t2.neighbors[i];
        const std::uint32_t n2 = t2.neighbors[cw(i)];
        // add new triangles and change existing ones
        using detail::arr3;
        t1 = triangle_t::make(arr3(v1, v2, v), arr3(n1, iTnew2, iTnew1));
        t2 = triangle_t::make(arr3(v3, v4, v), arr3(n3, iTnew1, iTnew2));
        triangles[iTnew1] = triangle_t::make(arr3(v1, v, v4), arr3(iT1, iT2, n4));
        triangles[iTnew2] = triangle_t::make(arr3(v3, v, v2), arr3(iT2, iT1, n2));
        // make and add new vertex
        add_adjacent_triangles(v, iT1, iTnew2, iT2, iTnew1);
        // adjust neighboring triangles and vertices
        change_neighbour(n4, iT1, iTnew1);
        change_neighbour(n2, iT2, iTnew2);
        add_adjacent_triangle(v1, iTnew1);
        add_adjacent_triangle(v3, iTnew2);
        remove_adjacent_triangle(v2, iT2);
        add_adjacent_triangle(v2, iTnew2);
        remove_adjacent_triangle(v4, iT1);
        add_adjacent_triangle(v4, iTnew1);
        // return newly added triangles
        std::stack<std::uint32_t> newTriangles;
        newTriangles.push(iT1);
        newTriangles.push(iTnew2);
        newTriangles.push(iT2);
        newTriangles.push(iTnew1);
        return newTriangles;
    }

    std::array<std::uint32_t, 2> trianglesAt(const vec2_<T>& pos) const
    {
        std::array<std::uint32_t, 2> out = { null_neighbour, null_neighbour };
        for (std::uint32_t i = std::uint32_t(0); i < std::uint32_t(triangles.size()); ++i) {
            const triangle_t& t = triangles[i];
            const vec2_<T>& v1 = vertices[t.vertices[0]];
            const vec2_<T>& v2 = vertices[t.vertices[1]];
            const vec2_<T>& v3 = vertices[t.vertices[2]];
            const point_to_triangle_location_t::Enum loc = locate_point_wrt_triangle(pos, v1, v2, v3);
            if (loc == point_to_triangle_location_t::OUTSIDE)
                continue;
            out[0] = i;
            if (check_on_edge(loc))
                out[1] = t.neighbors[edge_neighbour(loc)];
            return out;
        }
        throw std::runtime_error("No triangle was found at position");
    }

    /**
     * Ensure that triangulation conforms to constraints (fixed edges)
     * @note For each fixed edge that is not present in the triangulation its
     * midpoint is recursively added until the original edge is represented by a
     * sequence of its pieces. <b> New vertices are inserted.</b>
     * @note If some edge appears more than once the input this
     * means that multiple boundaries overlap at the edge and impacts how hole
     * detection algorithm of triangulator_t::erase_outer_triangles_and_holes works.
     * <b>Make sure there are no erroneous duplicates.</b>
     * @tparam TEdgeIter iterator that dereferences to custom edge type
     * @tparam TGetEdgeVertexStart function object getting start vertex index
     * from an edge.
     * Getter signature: const TEdgeIter::value_type& -> std::uint32_t
     * @tparam TGetEdgeVertexEnd function object getting end vertex index from
     * an edge. Getter signature: const TEdgeIter::value_type& -> std::uint32_t
     * @param first beginning of the range of edges to add
     * @param last end of the range of edges to add
     * @param getStart getter of edge start vertex index
     * @param getEnd getter of edge end vertex index
     */
    template <
        typename TEdgeIter,
        typename TGetEdgeVertexStart,
        typename TGetEdgeVertexEnd>
    void conform_to_edges(
        TEdgeIter first,
        TEdgeIter last,
        TGetEdgeVertexStart getStart,
        TGetEdgeVertexEnd getEnd);

    /*!
     * Returns:
     *  - intersected triangle index
     *  - index of point on the left of the line
     *  - index of point on the right of the line
     * If left point is right on the line: no triangle is intersected:
     *  - triangle index is no-neighbor (invalid)
     *  - index of point on the line
     *  - index of point on the right of the line
     */
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
    TintersectedTriangle(
        const std::uint32_t iA,
        const std::vector<std::uint32_t>& candidates,
        const vec2_<T>& a,
        const vec2_<T>& b,
        const T orientationTolerance = T(0)) const
    {
        typedef std::vector<std::uint32_t>::const_iterator TriIndCit;
        for (TriIndCit it = candidates.begin(); it != candidates.end(); ++it) {
            const std::uint32_t iT = *it;
            const triangle_t t = triangles[iT];
            const std::uint32_t i = get_vertex_index(t, iA);
            const std::uint32_t iP2 = t.vertices[ccw(i)];
            const T orientP2 = orient2D(vertices[iP2], a, b);
            const point_to_line_location_t::Enum locP2 = classify_orientation(orientP2);
            if (locP2 == point_to_line_location_t::RIGHT_SIDE) {
                const std::uint32_t iP1 = t.vertices[cw(i)];
                const T orientP1 = orient2D(vertices[iP1], a, b);
                const point_to_line_location_t::Enum locP1 = classify_orientation(orientP1);
                if (locP1 == point_to_line_location_t::COLLINEAR) {
                    return std::make_tuple(null_neighbour, iP1, iP1);
                }
                if (locP1 == point_to_line_location_t::LEFT_SIDE) {
                    if (orientationTolerance) {
                        T closestOrient;
                        std::uint32_t iClosestP;
                        if (std::abs(orientP1) <= std::abs(orientP2)) {
                            closestOrient = orientP1;
                            iClosestP = iP1;
                        } else {
                            closestOrient = orientP2;
                            iClosestP = iP2;
                        }
                        if (classify_orientation(
                                closestOrient, orientationTolerance)
                            == point_to_line_location_t::COLLINEAR) {
                            return std::make_tuple(null_neighbour, iClosestP, iClosestP);
                        }
                    }
                    return std::make_tuple(iT, iP1, iP2);
                }
            }
        }
        throw std::runtime_error("Could not find vertex triangle intersected by "
                                 "edge. Note: can be caused by duplicate points.");
    }

    /// Returns indices of three resulting triangles
    /* Insert point into triangle: split into 3 triangles:
     *  - create 2 new triangles
     *  - re-use old triangle for the 3rd
     *                      v3
     *                    / | \
     *                   /  |  \ <-- original triangle (t)
     *                  /   |   \
     *              n3 /    |    \ n2
     *                /newT2|newT1\
     *               /      v      \
     *              /    __/ \__    \
     *             /  __/       \__  \
     *            / _/      t'     \_ \
     *          v1 ___________________ v2
     *                     n1
     */
    std::stack<std::uint32_t> insert_point_in_triangle(
        const std::uint32_t v,
        const std::uint32_t iT)
    {
        const std::uint32_t iNewT1 = add_triangle();
        const std::uint32_t iNewT2 = add_triangle();

        triangle_t& t = triangles[iT];
        const std::array<std::uint32_t, 3> vv = t.vertices;
        const std::array<std::uint32_t, 3> nn = t.neighbors;
        const std::uint32_t v1 = vv[0], v2 = vv[1], v3 = vv[2];
        const std::uint32_t n1 = nn[0], n2 = nn[1], n3 = nn[2];
        // make two new triangles and convert current triangle to 3rd new
        // triangle
        using detail::arr3;
        triangles[iNewT1] = triangle_t::make(arr3(v2, v3, v), arr3(n2, iNewT2, iT));
        triangles[iNewT2] = triangle_t::make(arr3(v3, v1, v), arr3(n3, iT, iNewT1));
        t = triangle_t::make(arr3(v1, v2, v), arr3(n1, iNewT1, iNewT2));
        // make and add a new vertex
        add_adjacent_triangles(v, iT, iNewT1, iNewT2);
        // adjust lists of adjacent triangles for v1, v2, v3
        add_adjacent_triangle(v1, iNewT2);
        add_adjacent_triangle(v2, iNewT1);
        remove_adjacent_triangle(v3, iT);
        add_adjacent_triangle(v3, iNewT1);
        add_adjacent_triangle(v3, iNewT2);
        // change triangle neighbor's neighbors to new triangles
        change_neighbour(n2, iT, iNewT1);
        change_neighbour(n3, iT, iNewT2);
        // return newly added triangles
        std::stack<std::uint32_t> newTriangles;
        newTriangles.push(iT);
        newTriangles.push(iNewT1);
        newTriangles.push(iNewT2);
        return newTriangles;
    }

    std::array<std::uint32_t, 2> walking_search_triangle_at(
        const vec2_<T>& pos) const
    {
        std::array<std::uint32_t, 2> out = { null_neighbour, null_neighbour };
        // Query  for a vertex close to pos, to start the search
        const std::uint32_t startVertex = m_nearPtLocator.nearPoint(pos, vertices);
        const std::uint32_t iT = walk_triangles(startVertex, pos);
        // Finished walk, locate point in current triangle
        const triangle_t& t = triangles[iT];
        const vec2_<T>& v1 = vertices[t.vertices[0]];
        const vec2_<T>& v2 = vertices[t.vertices[1]];
        const vec2_<T>& v3 = vertices[t.vertices[2]];
        const point_to_triangle_location_t::Enum loc = locate_point_wrt_triangle(pos, v1, v2, v3);
        if (loc == point_to_triangle_location_t::OUTSIDE)
            throw std::runtime_error("No triangle was found at position");
        out[0] = iT;
        if (check_on_edge(loc))
            out[1] = t.neighbors[edge_neighbour(loc)];
        return out;
    }
    /**
     * Ensure that triangulation conforms to constraints (fixed edges)
     * @note For each fixed edge that is not present in the triangulation its
     * midpoint is recursively added until the original edge is represented by a
     * sequence of its pieces. <b> New vertices are inserted.</b>
     * @note If some edge appears more than once the input this
     * means that multiple boundaries overlap at the edge and impacts how hole
     * detection algorithm of triangulator_t::erase_outer_triangles_and_holes works.
     * <b>Make sure there are no erroneous duplicates.</b>
     * @tparam edges edges to conform to
     */
    void conform_to_edges(const std::vector<edge_t>& edges);
    /**
     * Erase triangles adjacent to super triangle
     *
     * @note does nothing if custom geometry is used
     */
    void eraseSuperTriangle();
    /// Erase triangles outside of constrained boundary using growing
    void erase_outer_triangles();
    /**
     * Erase triangles outside of constrained boundary and auto-detected holes
     *
     * @note detecting holes relies on layer peeling based on layer depth
     * @note supports overlapping or touching boundaries
     */
    void erase_outer_triangles_and_holes();
    /**
     * Call this method after directly setting custom super-geometry via
     * vertices and triangles members
     */
    void initialise_with_custom_supergeometry();

    /**
     * Check if the triangulation was finalized with `erase...` method and
     * super-triangle was removed.
     * @return true if triangulation is finalized, false otherwise
     */
    bool is_finalized() const;

    /**
     * Calculate depth of each triangle in constraint triangulation. Supports
     * overlapping boundaries.
     *
     * Perform depth peeling from super triangle to outermost boundary,
     * then to next boundary and so on until all triangles are traversed.@n
     * For example depth is:
     *  - 0 for triangles outside outermost boundary
     *  - 1 for triangles inside boundary but outside hole
     *  - 2 for triangles in hole
     *  - 3 for triangles in island and so on...
     * @return vector where element at index i stores depth of i-th triangle
     */

    std::vector<layer_depth_t>
    calculate_triangle_depths() const
    {
        std::vector<layer_depth_t> triDepths(
            triangles.size(), std::numeric_limits<layer_depth_t>::max());
        std::stack<std::uint32_t> seeds(std::deque<std::uint32_t>(1, vertTris[0].front()));
        layer_depth_t layerDepth = 0;
        layer_depth_t deepestSeedDepth = 0;

        std::unordered_map<layer_depth_t, std::unordered_set<std::uint32_t>> seedsByDepth;
        
        do {
            const std::unordered_map<std::uint32_t, layer_depth_t>& newSeeds = peel_layer(seeds, layerDepth, triDepths);

            seedsByDepth.erase(layerDepth);
            typedef std::unordered_map<std::uint32_t, layer_depth_t>::const_iterator Iter;
            for (Iter it = newSeeds.begin(); it != newSeeds.end(); ++it) {
                deepestSeedDepth = std::max(deepestSeedDepth, it->second);
                seedsByDepth[it->second].insert(it->first);
            }
            const std::unordered_set<std::uint32_t>& nextLayerSeeds = seedsByDepth[layerDepth + 1];
            seeds = std::stack<std::uint32_t>(
                std::deque<std::uint32_t>(nextLayerSeeds.begin(), nextLayerSeeds.end()));
            ++layerDepth;
        } while (!seeds.empty() || deepestSeedDepth > layerDepth);

        return triDepths;
    }

    /**
     * @defgroup Advanced Advanced triangulator_t Methods
     * Advanced methods for manually modifying the triangulation from
     * outside. Please only use them when you know what you are doing.
     */
    /// @{

    /**
     * Flip an edge between two triangle.
     * @note Advanced method for manually modifying the triangulation from
     * outside. Please call it when you know what you are doing.
     * @param iT first triangle
     * @param iTopo second triangle

     */
    void do_edgeflip(std::uint32_t iT, std::uint32_t iTopo);

    /**
     * Remove triangles with specified indices.
     * Adjust internal triangulation state accordingly.
     * @param removedTriangles indices of triangles to remove
     */
    void remove_triangles(const std::unordered_set<std::uint32_t>& removedTriangles);
    /// @}

private:
    /*____ Detail __*/
    void add_super_triangle(const box2d_t<T>& box);
    void create_vertex(const vec2_<T>& pos, const std::vector<std::uint32_t>& tris);
    void insert_vertex(std::uint32_t iVert);
    void enforce_delaunay_property_using_edge_flips(
        const vec2_<T>& v,
        std::uint32_t iVert,
        std::stack<std::uint32_t>& triStack);
    /// Flip fixed edges and return a list of flipped fixed edges
    std::vector<edge_t> insert_vertex_and_flip_fixed_edges(std::uint32_t iVert);
    /**
     * Insert an edge into constraint Delaunay triangulation
     * @param edge edge to insert
     * @param originalEdge original edge inserted edge is part of
     */
    void insert_edge(edge_t edge, edge_t originalEdge);
    /**
     * Conform Delaunay triangulation to a fixed edge by recursively inserting
     * mid point of the edge and then conforming to its halves
     * @param edge fixed edge to conform to
     * @param originalEdges original edges that new edge is piece of
     * @param overlaps count of overlapping boundaries at the edge. Only used
     * when re-introducing edge with overlaps > 0
     * @param orientationTolerance tolerance for orient2d predicate,
     * values [-tolerance,+tolerance] are considered as 0.
     */
    void conform_to_edge(
        edge_t edge,
        std::vector<edge_t> originalEdges,
        boundary_overlap_count_t overlaps);

    std::uint32_t walk_triangles(std::uint32_t startVertex, const vec2_<T>& pos) const;
    bool check_is_edgeflip_needed(
        const vec2_<T>& v,
        std::uint32_t iV,
        std::uint32_t iV1,
        std::uint32_t iV2,
        std::uint32_t iV3) const;
    bool
    check_is_edgeflip_needed(const vec2_<T>& v, std::uint32_t iT, std::uint32_t iTopo, std::uint32_t iVert) const;
    void change_neighbour(std::uint32_t iT, std::uint32_t oldNeighbor, std::uint32_t newNeighbor);
    void change_neighbour(
        std::uint32_t iT,
        std::uint32_t iVedge1,
        std::uint32_t iVedge2,
        std::uint32_t newNeighbor);
    void add_adjacent_triangle(std::uint32_t iVertex, std::uint32_t iTriangle);
    void
    add_adjacent_triangles(std::uint32_t iVertex, std::uint32_t iT1, std::uint32_t iT2, std::uint32_t iT3);
    void add_adjacent_triangles(
        std::uint32_t iVertex,
        std::uint32_t iT1,
        std::uint32_t iT2,
        std::uint32_t iT3,
        std::uint32_t iT4);
    void remove_adjacent_triangle(std::uint32_t iVertex, std::uint32_t iTriangle);
    std::uint32_t triangulate_pseudo_polygon(
        std::uint32_t ia,
        std::uint32_t ib,
        std::vector<std::uint32_t>::const_iterator pointsFirst,
        std::vector<std::uint32_t>::const_iterator pointsLast);
    std::uint32_t find_delaunay_point(
        std::uint32_t ia,
        std::uint32_t ib,
        std::vector<std::uint32_t>::const_iterator pointsFirst,
        std::vector<std::uint32_t>::const_iterator pointsLast) const;
    std::uint32_t pseudo_polygon_outer_triangle(std::uint32_t ia, std::uint32_t ib) const;
    std::uint32_t add_triangle(const triangle_t& t); // note: invalidates iterators!
    std::uint32_t add_triangle(); // note: invalidates triangle iterators!
    /**
     * Remove super-triangle (if used) and triangles with specified indices.
     * Adjust internal triangulation state accordingly.
     * @removedTriangles indices of triangles to remove
     */
    void finalise_triangulation(const std::unordered_set<std::uint32_t>& removedTriangles);
    std::unordered_set<std::uint32_t> grow_to_boundary(std::stack<std::uint32_t> seeds) const;

    void fixEdge(
        const edge_t& edge,
        const boundary_overlap_count_t overlaps)
    {
        fixedEdges.insert(edge);
        overlapCount[edge] = overlaps; // override overlap counter
    }

    void fixEdge(const edge_t& edge)
    {
        if (!fixedEdges.insert(edge).second) {
            ++overlapCount[edge]; // if edge is already fixed increment the counter
        }
    }

    void fixEdge(
        const edge_t& edge,
        const edge_t& originalEdge)
    {
        fixEdge(edge);
        if (edge != originalEdge)
            detail::insert_unique(pieceToOriginals[edge], originalEdge);
    }
    /**
     * Flag triangle as dummy
     * @note Advanced method for manually modifying the triangulation from
     * outside. Please call it when you know what you are doing.
     * @param iT index of a triangle to flag
     */
    void make_dummies(const std::uint32_t iT)
    {
        const triangle_t& t = triangles[iT];

        typedef std::array<std::uint32_t, 3>::const_iterator VCit;
        for (VCit iV = t.vertices.begin(); iV != t.vertices.end(); ++iV)
            remove_adjacent_triangle(*iV, iT);

        typedef std::array<std::uint32_t, 3>::const_iterator NCit;
        for (NCit iTn = t.neighbors.begin(); iTn != t.neighbors.end(); ++iTn)
            change_neighbour(*iTn, iT, null_neighbour);

        m_dummyTris.push_back(iT);
    }
    /**
     * Erase all dummy triangles
     * @note Advanced method for manually modifying the triangulation from
     * outside. Please call it when you know what you are doing.
     */
    void erase_dummies()
    {
        if (m_dummyTris.empty())
            return;
        const std::unordered_set<std::uint32_t> dummySet(m_dummyTris.begin(), m_dummyTris.end());
        std::unordered_map<std::uint32_t, std::uint32_t> triIndMap;
        triIndMap[null_neighbour] = null_neighbour;
        for (std::uint32_t iT(0), iTnew(0); iT < std::uint32_t(triangles.size()); ++iT) {
            if (dummySet.count(iT))
                continue;
            triIndMap[iT] = iTnew;
            triangles[iTnew] = triangles[iT];
            iTnew++;
        }
        triangles.erase(triangles.end() - dummySet.size(), triangles.end());

        // remap adjacent triangle indices for vertices
        typedef typename std::vector<std::vector<std::uint32_t>>::iterator VertTrisIt;
        for (VertTrisIt vTris = vertTris.begin(); vTris != vertTris.end(); ++vTris) {
            for (std::vector<std::uint32_t>::iterator iT = vTris->begin(); iT != vTris->end(); ++iT)
                *iT = triIndMap[*iT];
        }
        // remap neighbor indices for triangles
        for (std::vector<triangle_t>::iterator t = triangles.begin(); t != triangles.end(); ++t) {
            std::array<std::uint32_t, 3>& nn = t->neighbors;
            for (std::array<std::uint32_t, 3>::iterator iN = nn.begin(); iN != nn.end(); ++iN)
                *iN = triIndMap[*iN];
        }
        // clear dummy triangles
        m_dummyTris = std::vector<std::uint32_t>();
    }

    /**
     * Depth-peel a layer in triangulation, used when calculating triangle
     * depths
     *
     * It takes starting seed triangles, traverses neighboring triangles, and
     * assigns given layer depth to the traversed triangles. Traversal is
     * blocked by constraint edges. Triangles behind constraint edges are
     * recorded as seeds of next layer and returned from the function.
     *
     * @param seeds indices of seed triangles
     * @param layerDepth current layer's depth to mark triangles with
     * @param[in, out] triDepths depths of triangles
     * @return triangles of the deeper layers that are adjacent to the peeled
     * layer. To be used as seeds when peeling deeper layers.
     */
    std::unordered_map<std::uint32_t, layer_depth_t>
    peel_layer(
        std::stack<std::uint32_t> seeds,
        const layer_depth_t layerDepth,
        std::vector<layer_depth_t>& triDepths) const
    {
        std::unordered_map<std::uint32_t, layer_depth_t> behindBoundary;
        while (!seeds.empty()) {
            const std::uint32_t iT = seeds.top();
            seeds.pop();
            triDepths[iT] = layerDepth;
            behindBoundary.erase(iT);
            const triangle_t& t = triangles[iT];
            for (std::uint32_t i(0); i < std::uint32_t(3); ++i) {
                const edge_t opEdge(t.vertices[ccw(i)], t.vertices[cw(i)]);
                const std::uint32_t iN = t.neighbors[get_opposite_neighbour_from_vertex(i)];
                if (iN == null_neighbour || triDepths[iN] <= layerDepth)
                    continue;
                if (fixedEdges.count(opEdge)) {
                    const std::unordered_map<edge_t, layer_depth_t>::const_iterator cit = overlapCount.find(opEdge);
                    const layer_depth_t triDepth = cit == overlapCount.end()
                        ? layerDepth + 1
                        : layerDepth + cit->second + 1;
                    behindBoundary[iN] = triDepth;
                    continue;
                }
                seeds.push(iN);
            }
        }
        return behindBoundary;
    }

    std::vector<std::uint32_t> m_dummyTris;
    TNearPointLocator m_nearPtLocator;
    std::size_t m_nTargetVerts;
    super_geometry_type_t::Enum m_superGeomType;
    vertex_insertion_order_t::Enum m_vertexInsertionOrder;
    action_on_intersecting_constraint_edges_t::Enum m_intersectingEdgesStrategy;
    T m_minDistToConstraintEdge;
};

/// @}
/// @}

namespace detail {

    static std::mt19937 randGenerator(9001);

    template <class RandomIt>
    void random_shuffle(RandomIt first, RandomIt last)
    {
        typename std::iterator_traits<RandomIt>::difference_type i, n;
        n = last - first;
        for (i = n - 1; i > 0; --i) {
            std::swap(first[i], first[randGenerator() % (i + 1)]);
        }
    }

} // namespace detail

//-----------------------
// triangulator_t methods
//-----------------------
template <typename T, typename TNearPointLocator>
template <
    typename TVertexIter,
    typename TGetVertexCoordX,
    typename TGetVertexCoordY>
void triangulator_t<T, TNearPointLocator>::insert_vertices(
    const TVertexIter first,
    const TVertexIter last,
    TGetVertexCoordX get_x_coord,
    TGetVertexCoordY get_y_coord)
{
    if (is_finalized()) {
        throw std::runtime_error(
            "triangulator_t was finalized with 'erase...' method. Inserting new "
            "vertices is not possible");
    }

    detail::randGenerator.seed(9001); // ensure deterministic behavior

    if (vertices.empty()) {
        add_super_triangle(expand_with_points<T>(first, last, get_x_coord, get_y_coord));
    }

    const std::size_t nExistingVerts = vertices.size();

    vertices.reserve(nExistingVerts + std::distance(first, last));

    for (TVertexIter it = first; it != last; ++it) {
        create_vertex(vec2_<T>::make(get_x_coord(*it), get_y_coord(*it)), std::vector<std::uint32_t>());
    }

    switch (m_vertexInsertionOrder) {

    case vertex_insertion_order_t::AS_GIVEN: {

        for (TVertexIter it = first; it != last; ++it) {
            insert_vertex(std::uint32_t(nExistingVerts + std::distance(first, it)));
        }

        break;
    }
    case vertex_insertion_order_t::RANDOM: {
        std::vector<std::uint32_t> ii(std::distance(first, last));
        typedef std::vector<std::uint32_t>::iterator Iter;
        std::uint32_t value = static_cast<std::uint32_t>(nExistingVerts);
        for (Iter it = ii.begin(); it != ii.end(); ++it, ++value)
            *it = value;
        detail::random_shuffle(ii.begin(), ii.end());
        for (Iter it = ii.begin(); it != ii.end(); ++it)
            insert_vertex(*it);
        break;
    }
    }
}

template <typename T, typename TNearPointLocator>
template <
    typename TEdgeIter,
    typename TGetEdgeVertexStart,
    typename TGetEdgeVertexEnd>
void triangulator_t<T, TNearPointLocator>::insert_edges(
    TEdgeIter first,
    const TEdgeIter last,
    TGetEdgeVertexStart getStart,
    TGetEdgeVertexEnd getEnd)
{
    if (is_finalized()) {
        throw std::runtime_error(
            "triangulator_t was finalized with 'erase...' method. Inserting new "
            "edges is not possible");
    }
    for (; first != last; ++first) {
        // +3 to account for super-triangle vertices
        const edge_t edge(
            std::uint32_t(getStart(*first) + m_nTargetVerts),
            std::uint32_t(getEnd(*first) + m_nTargetVerts));
        insert_edge(edge, edge);
    }
    erase_dummies();
}

template <typename T, typename TNearPointLocator>
template <
    typename TEdgeIter,
    typename TGetEdgeVertexStart,
    typename TGetEdgeVertexEnd>
void triangulator_t<T, TNearPointLocator>::conform_to_edges(
    TEdgeIter first,
    const TEdgeIter last,
    TGetEdgeVertexStart getStart,
    TGetEdgeVertexEnd getEnd)
{
    if (is_finalized()) {
        throw std::runtime_error(
            "triangulator_t was finalized with 'erase...' method. Conforming to "
            "new edges is not possible");
    }
    for (; first != last; ++first) {
        // +3 to account for super-triangle vertices
        const edge_t e(
            std::uint32_t(getStart(*first) + m_nTargetVerts),
            std::uint32_t(getEnd(*first) + m_nTargetVerts));
        conform_to_edge(e, std::vector<edge_t>(1, e), 0);
    }
    erase_dummies();
}

} // namespace cdt

#ifndef CDT_USE_AS_COMPILED_LIBRARY
//#include "triangulator_t.hpp"

#include <algorithm>
#include <cassert>
#include <deque>
#include <stdexcept>

namespace cdt {

template <typename T, typename TNearPointLocator>
triangulator_t<T, TNearPointLocator>::triangulator_t()
    : m_nTargetVerts(detail::defaults::nTargetVerts)
    , m_superGeomType(detail::defaults::superGeomType)
    , m_vertexInsertionOrder(detail::defaults::vertexInsertionOrder)
    , m_intersectingEdgesStrategy(detail::defaults::intersectingEdgesStrategy)
    , m_minDistToConstraintEdge(detail::defaults::minDistToConstraintEdge)
{
}

template <typename T, typename TNearPointLocator>
triangulator_t<T, TNearPointLocator>::triangulator_t(
    const vertex_insertion_order_t::Enum vertexInsertionOrder)
    : m_nTargetVerts(detail::defaults::nTargetVerts)
    , m_superGeomType(detail::defaults::superGeomType)
    , m_vertexInsertionOrder(vertexInsertionOrder)
    , m_intersectingEdgesStrategy(detail::defaults::intersectingEdgesStrategy)
    , m_minDistToConstraintEdge(detail::defaults::minDistToConstraintEdge)
{
}

template <typename T, typename TNearPointLocator>
triangulator_t<T, TNearPointLocator>::triangulator_t(
    const vertex_insertion_order_t::Enum vertexInsertionOrder,
    const action_on_intersecting_constraint_edges_t::Enum intersectingEdgesStrategy,
    const T minDistToConstraintEdge)
    : m_nTargetVerts(detail::defaults::nTargetVerts)
    , m_superGeomType(detail::defaults::superGeomType)
    , m_vertexInsertionOrder(vertexInsertionOrder)
    , m_intersectingEdgesStrategy(intersectingEdgesStrategy)
    , m_minDistToConstraintEdge(minDistToConstraintEdge)
{
}

template <typename T, typename TNearPointLocator>
triangulator_t<T, TNearPointLocator>::triangulator_t(
    const vertex_insertion_order_t::Enum vertexInsertionOrder,
    const TNearPointLocator& nearPtLocator,
    const action_on_intersecting_constraint_edges_t::Enum intersectingEdgesStrategy,
    const T minDistToConstraintEdge)
    : m_nTargetVerts(detail::defaults::nTargetVerts)
    , m_nearPtLocator(nearPtLocator)
    , m_superGeomType(detail::defaults::superGeomType)
    , m_vertexInsertionOrder(vertexInsertionOrder)
    , m_intersectingEdgesStrategy(intersectingEdgesStrategy)
    , m_minDistToConstraintEdge(minDistToConstraintEdge)
{
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::change_neighbour(
    const std::uint32_t iT,
    const std::uint32_t iVedge1,
    const std::uint32_t iVedge2,
    const std::uint32_t newNeighbor)
{
    triangle_t& t = triangles[iT];
    t.neighbors[opposite_triangle_index(t, iVedge1, iVedge2)] = newNeighbor;
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::eraseSuperTriangle()
{
    if (m_superGeomType != super_geometry_type_t::SUPER_TRIANGLE)
        return;
    // find triangles adjacent to super-triangle's vertices
    std::unordered_set<std::uint32_t> toErase;
    toErase.reserve(
        vertTris[0].size() + vertTris[1].size() + vertTris[2].size());
    for (std::uint32_t iT(0); iT < std::uint32_t(triangles.size()); ++iT) {
        triangle_t& t = triangles[iT];
        if (t.vertices[0] < 3 || t.vertices[1] < 3 || t.vertices[2] < 3)
            toErase.insert(iT);
    }
    finalise_triangulation(toErase);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::erase_outer_triangles()
{
    // make dummy triangles adjacent to super-triangle's vertices
    const std::stack<std::uint32_t> seed(std::deque<std::uint32_t>(1, vertTris[0].front()));
    const std::unordered_set<std::uint32_t> toErase = grow_to_boundary(seed);
    finalise_triangulation(toErase);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::erase_outer_triangles_and_holes()
{
    const std::vector<layer_depth_t> triDepths = calculate_triangle_depths();
    std::unordered_set<std::uint32_t> toErase;
    toErase.reserve(triangles.size());

    for (std::size_t iT = 0; iT != triangles.size(); ++iT) {
        if (triDepths[iT] % 2 == 0){
            toErase.insert(static_cast<std::uint32_t>(iT));
        }
    }

    finalise_triangulation(toErase);
}

/// Remap removing super-triangle: subtract 3 from vertices
inline edge_t remap_no_supertriangle(const edge_t& e)
{
    return edge_t(e.v1() - 3, e.v2() - 3);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::remove_triangles(
    const std::unordered_set<std::uint32_t>& removedTriangles)
{
    if (removedTriangles.empty())
        return;
    // remove triangles and calculate triangle index mapping
    std::unordered_map<std::uint32_t, std::uint32_t> triIndMap;
    for (std::uint32_t iT(0), iTnew(0); iT < std::uint32_t(triangles.size()); ++iT) {
        if (removedTriangles.count(iT))
            continue;
        triIndMap[iT] = iTnew;
        triangles[iTnew] = triangles[iT];
        iTnew++;
    }
    triangles.erase(triangles.end() - removedTriangles.size(), triangles.end());
    // adjust triangles' neighbors
    vertTris = std::vector<std::vector<std::uint32_t>>();
    for (std::uint32_t iT = 0; iT < triangles.size(); ++iT) {
        triangle_t& t = triangles[iT];
        // update neighbors to account for removed triangles
        std::array<std::uint32_t, 3>& nn = t.neighbors;
        for (std::array<std::uint32_t, 3>::iterator n = nn.begin(); n != nn.end(); ++n) {
            if (removedTriangles.count(*n)) {
                *n = null_neighbour;
            } else if (*n != null_neighbour) {
                *n = triIndMap[*n];
            }
        }
    }
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::finalise_triangulation(
    const std::unordered_set<std::uint32_t>& removedTriangles)
{
    erase_dummies();
    // remove super-triangle
    if (m_superGeomType == super_geometry_type_t::SUPER_TRIANGLE) {
        vertices.erase(vertices.begin(), vertices.begin() + 3);
        if (removedTriangles.empty())
            vertTris.erase(vertTris.begin(), vertTris.begin() + 3);
        // edge_t re-mapping
        { // fixed edges
            std::unordered_set<edge_t> updatedFixedEdges;
            typedef std::unordered_set<edge_t>::const_iterator It;
            for (It e = fixedEdges.begin(); e != fixedEdges.end(); ++e) {
                updatedFixedEdges.insert(remap_no_supertriangle(*e));
            }
            fixedEdges = updatedFixedEdges;
        }
        { // overlap count
            std::unordered_map<edge_t, boundary_overlap_count_t> updatedOverlapCount;
            typedef std::unordered_map<edge_t, boundary_overlap_count_t>::const_iterator
                It;
            for (It it = overlapCount.begin(); it != overlapCount.end(); ++it) {
                updatedOverlapCount.insert(std::make_pair(
                    remap_no_supertriangle(it->first), it->second));
            }
            overlapCount = updatedOverlapCount;
        }
        { // split edges mapping
            std::unordered_map<edge_t, std::vector<edge_t>> updatedPieceToOriginals;
            typedef std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator It;
            for (It it = pieceToOriginals.begin(); it != pieceToOriginals.end();
                 ++it) {
                std::vector<edge_t> ee = it->second;
                for (std::vector<edge_t>::iterator eeIt = ee.begin(); eeIt != ee.end();
                     ++eeIt) {
                    *eeIt = remap_no_supertriangle(*eeIt);
                }
                updatedPieceToOriginals.insert(
                    std::make_pair(remap_no_supertriangle(it->first), ee));
            }
            pieceToOriginals = updatedPieceToOriginals;
        }
    }
    // remove other triangles
    remove_triangles(removedTriangles);
    // adjust triangle vertices: account for removed super-triangle
    if (m_superGeomType == super_geometry_type_t::SUPER_TRIANGLE) {
        for (std::vector<triangle_t>::iterator t = triangles.begin(); t != triangles.end();
             ++t) {
            std::array<std::uint32_t, 3>& vv = t->vertices;
            for (std::array<std::uint32_t, 3>::iterator v = vv.begin(); v != vv.end(); ++v) {
                *v -= 3;
            }
        }
    }
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::initialise_with_custom_supergeometry()
{
    m_nearPtLocator.initialize(vertices);
    m_nTargetVerts = vertices.size();
    m_superGeomType = super_geometry_type_t::CUSTOM;
}

template <typename T, typename TNearPointLocator>
std::unordered_set<std::uint32_t> triangulator_t<T, TNearPointLocator>::grow_to_boundary(
    std::stack<std::uint32_t> seeds) const
{
    std::unordered_set<std::uint32_t> traversed;
    while (!seeds.empty()) {
        const std::uint32_t iT = seeds.top();
        seeds.pop();
        traversed.insert(iT);
        const triangle_t& t = triangles[iT];
        for (std::uint32_t i(0); i < std::uint32_t(3); ++i) {
            const edge_t opEdge(t.vertices[ccw(i)], t.vertices[cw(i)]);
            if (fixedEdges.count(opEdge))
                continue;
            const std::uint32_t iN = t.neighbors[get_opposite_neighbour_from_vertex(i)];
            if (iN != null_neighbour && traversed.count(iN) == 0)
                seeds.push(iN);
        }
    }
    return traversed;
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::add_triangle(const triangle_t& t)
{
    if (m_dummyTris.empty()) {
        triangles.push_back(t);
        return std::uint32_t(triangles.size() - 1);
    }
    const std::uint32_t nxtDummy = m_dummyTris.back();
    m_dummyTris.pop_back();
    triangles[nxtDummy] = t;
    return nxtDummy;
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::add_triangle()
{
    if (m_dummyTris.empty()) {
        const triangle_t dummy = {
            { null_vertex, null_vertex, null_vertex },
            { null_neighbour, null_neighbour, null_neighbour }
        };
        triangles.push_back(dummy);
        return std::uint32_t(triangles.size() - 1);
    }
    const std::uint32_t nxtDummy = m_dummyTris.back();
    m_dummyTris.pop_back();
    return nxtDummy;
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::insert_edges(
    const std::vector<edge_t>& edges)
{
    insert_edges(edges.begin(), edges.end(), edge_get_v1, edge_get_v2);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::conform_to_edges(
    const std::vector<edge_t>& edges)
{
    conform_to_edges(edges.begin(), edges.end(), edge_get_v1, edge_get_v2);
}

namespace detail {

    template <typename T>
    T lerp(const T& a, const T& b, const T t)
    {
        return (T(1) - t) * a + t * b;
    }

    // Precondition: ab and cd intersect normally
    template <typename T>
    vec2_<T> get_intersection_point_coords(
        const vec2_<T>& a,
        const vec2_<T>& b,
        const vec2_<T>& c,
        const vec2_<T>& d)
    {
        // interpolate point on the shorter segment
        if (get_square_distance(a, b) < get_square_distance(c, d)) {
            // const T a_cd = orient2d(c.x(), c.y(), d.x(), d.y(), a.x(), a.y());
            // const T b_cd = orient2d(c.x(), c.y(), d.x(), d.y(), b.x(), b.y());
            const T a_cd = orient2d(c, d, a);
            const T b_cd = orient2d(c, d, b);
            const T t = a_cd / (a_cd - b_cd);
            return vec2_<T>::make(lerp(a.x(), b.x(), t), lerp(a.y(), b.y(), t));
        } else {
            // const T c_ab = orient2d(a.x(), a.y(), b.x(), b.y(), c.x(), c.y());
            // const T d_ab = orient2d(a.x(), a.y(), b.x(), b.y(), d.x(), d.y());
            const T c_ab = orient2d(a, b, c);
            const T d_ab = orient2d(a, b, d);
            const T t = c_ab / (c_ab - d_ab);
            return vec2_<T>::make(lerp(c.x(), d.x(), t), lerp(c.y(), d.y(), t));
        }
    }

} // namespace detail

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::insert_edge(
    const edge_t edge,
    const edge_t originalEdge)
{
    const std::uint32_t iA = edge.v1();
    std::uint32_t iB = edge.v2();
    if (iA == iB) // edge connects a vertex to itself
        return;
    const std::vector<std::uint32_t>& aTris = vertTris[iA];
    const std::vector<std::uint32_t>& bTris = vertTris[iB];
    const vec2_<T>& a = vertices[iA];
    const vec2_<T>& b = vertices[iB];
    if (check_vertices_share_edge(aTris, bTris)) {
        fixEdge(edge, originalEdge);
        return;
    }

    const T distanceTolerance = m_minDistToConstraintEdge == T(0)
        ? T(0)
        : m_minDistToConstraintEdge * distance(a, b);

    std::uint32_t iT;
    std::uint32_t iVleft, iVright;
    std::tie(iT, iVleft, iVright) = get_intersected_triangle(iA, aTris, a, b, distanceTolerance);
    // if one of the triangle vertices is on the edge, move edge start
    if (iT == null_neighbour) {
        const edge_t edgePart(iA, iVleft);
        fixEdge(edgePart, originalEdge);
        return insert_edge(edge_t(iVleft, iB), originalEdge);
    }
    std::vector<std::uint32_t> intersected(1, iT);
    std::vector<std::uint32_t> ptsLeft(1, iVleft);
    std::vector<std::uint32_t> ptsRight(1, iVright);
    std::uint32_t iV = iA;
    triangle_t t = triangles[iT];
    while (std::find(t.vertices.begin(), t.vertices.end(), iB) == t.vertices.end()) {
        const std::uint32_t iTopo = get_opposite_triangle_index(t, iV);
        const triangle_t& tOpo = triangles[iTopo];
        const std::uint32_t iVopo = get_opposed_vertex_index(tOpo, iT);
        const vec2_<T> vOpo = vertices[iVopo];

        // RESOLVE intersection between two constraint edges if needed
        if (m_intersectingEdgesStrategy == action_on_intersecting_constraint_edges_t::RESOLVE && fixedEdges.count(edge_t(iVleft, iVright))) {
            const std::uint32_t iNewVert = static_cast<std::uint32_t>(vertices.size());

            // split constraint edge that already exists in triangulation
            const edge_t splitEdge(iVleft, iVright);
            const edge_t half1(iVleft, iNewVert);
            const edge_t half2(iNewVert, iVright);
            const boundary_overlap_count_t overlaps = overlapCount[splitEdge];
            // remove the edge that will be split
            fixedEdges.erase(splitEdge);
            overlapCount.erase(splitEdge);
            // add split edge's halves
            fixEdge(half1, overlaps);
            fixEdge(half2, overlaps);
            // maintain piece-to-original mapping
            std::vector<edge_t> newOriginals(1, splitEdge);
            const std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator originalsIt = pieceToOriginals.find(splitEdge);
            if (originalsIt != pieceToOriginals.end()) { // edge being split was split before: pass-through originals
                newOriginals = originalsIt->second;
                pieceToOriginals.erase(originalsIt);
            }
            detail::insert_unique(pieceToOriginals[half1], newOriginals);
            detail::insert_unique(pieceToOriginals[half2], newOriginals);

            // add a new point at the intersection of two constraint edges
            const vec2_<T> newV = detail::get_intersection_point_coords(
                vertices[iA],
                vertices[iB],
                vertices[iVleft],
                vertices[iVright]);
            create_vertex(newV, std::vector<std::uint32_t>());
            std::stack<std::uint32_t> triStack = insert_point_on_edge(iNewVert, iT, iTopo);
            enforce_delaunay_property_using_edge_flips(newV, iNewVert, triStack);
            // TODO: is it's possible to re-use pseudo-polygons
            //  for inserting [iA, iNewVert] edge half?
            insert_edge(edge_t(iA, iNewVert), originalEdge);
            insert_edge(edge_t(iNewVert, iB), originalEdge);
            return;
        }

        intersected.push_back(iTopo);
        iT = iTopo;
        t = triangles[iT];

        const point_to_line_location_t::Enum loc = locate_point_wrt_line(vOpo, a, b, distanceTolerance);
        if (loc == point_to_line_location_t::LEFT_SIDE) {
            ptsLeft.push_back(iVopo);
            iV = iVleft;
            iVleft = iVopo;
        } else if (loc == point_to_line_location_t::RIGHT_SIDE) {
            ptsRight.push_back(iVopo);
            iV = iVright;
            iVright = iVopo;
        } else // encountered point on the edge
            iB = iVopo;
    }
    // Remove intersected triangles
    typedef std::vector<std::uint32_t>::const_iterator TriIndCit;
    for (TriIndCit it = intersected.begin(); it != intersected.end(); ++it)
        make_dummies(*it);
    // Triangulate pseudo-polygons on both sides
    const std::uint32_t iTleft = triangulate_pseudo_polygon(iA, iB, ptsLeft.begin(), ptsLeft.end());
    std::reverse(ptsRight.begin(), ptsRight.end());
    const std::uint32_t iTright = triangulate_pseudo_polygon(iB, iA, ptsRight.begin(), ptsRight.end());
    change_neighbour(iTleft, null_neighbour, iTright);
    change_neighbour(iTright, null_neighbour, iTleft);

    if (iB != edge.v2()) // encountered point on the edge
    {
        // fix edge part
        const edge_t edgePart(iA, iB);
        fixEdge(edgePart, originalEdge);
        return insert_edge(edge_t(iB, edge.v2()), originalEdge);
    } else {
        fixEdge(edge, originalEdge);
    }
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::conform_to_edge(
    const edge_t edge,
    std::vector<edge_t> originalEdges,
    const boundary_overlap_count_t overlaps)
{
    const std::uint32_t iA = edge.v1();
    std::uint32_t iB = edge.v2();
    if (iA == iB) // edge connects a vertex to itself
        return;
    const std::vector<std::uint32_t>& aTris = vertTris[iA];
    const std::vector<std::uint32_t>& bTris = vertTris[iB];
    const vec2_<T>& a = vertices[iA];
    const vec2_<T>& b = vertices[iB];
    if (check_vertices_share_edge(aTris, bTris)) {
        overlaps > 0 ? fixEdge(edge, overlaps) : fixEdge(edge);
        // avoid marking edge as a part of itself
        if (!originalEdges.empty() && edge != originalEdges.front()) {
            detail::insert_unique(pieceToOriginals[edge], originalEdges);
        }
        return;
    }

    const T distanceTolerance = m_minDistToConstraintEdge == T(0)
        ? T(0)
        : m_minDistToConstraintEdge * distance(a, b);
    std::uint32_t iT;
    std::uint32_t iVleft, iVright;
    std::tie(iT, iVleft, iVright) = get_intersected_triangle(iA, aTris, a, b, distanceTolerance);
    // if one of the triangle vertices is on the edge, move edge start
    if (iT == null_neighbour) {
        const edge_t edgePart(iA, iVleft);
        overlaps > 0 ? fixEdge(edgePart, overlaps) : fixEdge(edgePart);
        detail::insert_unique(pieceToOriginals[edgePart], originalEdges);
        return conform_to_edge(edge_t(iVleft, iB), originalEdges, overlaps);
    }

    std::uint32_t iV = iA;
    triangle_t t = triangles[iT];
    while (std::find(t.vertices.begin(), t.vertices.end(), iB) == t.vertices.end()) {
        const std::uint32_t iTopo = get_opposite_triangle_index(t, iV);
        const triangle_t& tOpo = triangles[iTopo];
        const std::uint32_t iVopo = get_opposed_vertex_index(tOpo, iT);
        const vec2_<T> vOpo = vertices[iVopo];

        // RESOLVE intersection between two constraint edges if needed
        if (m_intersectingEdgesStrategy == action_on_intersecting_constraint_edges_t::RESOLVE && fixedEdges.count(edge_t(iVleft, iVright))) {
            const std::uint32_t iNewVert = static_cast<std::uint32_t>(vertices.size());

            // split constraint edge that already exists in triangulation
            const edge_t splitEdge(iVleft, iVright);
            const edge_t half1(iVleft, iNewVert);
            const edge_t half2(iNewVert, iVright);
            const boundary_overlap_count_t overlaps = overlapCount[splitEdge];
            // remove the edge that will be split
            fixedEdges.erase(splitEdge);
            overlapCount.erase(splitEdge);
            // add split edge's halves
            fixEdge(half1, overlaps);
            fixEdge(half2, overlaps);
            // maintain piece-to-original mapping
            std::vector<edge_t> newOriginals(1, splitEdge);
            const std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator originalsIt = pieceToOriginals.find(splitEdge);
            if (originalsIt != pieceToOriginals.end()) { // edge being split was split before: pass-through originals
                newOriginals = originalsIt->second;
                pieceToOriginals.erase(originalsIt);
            }
            detail::insert_unique(pieceToOriginals[half1], newOriginals);
            detail::insert_unique(pieceToOriginals[half2], newOriginals);

            // add a new point at the intersection of two constraint edges
            const vec2_<T> newV = detail::get_intersection_point_coords(
                vertices[iA],
                vertices[iB],
                vertices[iVleft],
                vertices[iVright]);
            create_vertex(newV, std::vector<std::uint32_t>());
            std::stack<std::uint32_t> triStack = insert_point_on_edge(iNewVert, iT, iTopo);
            enforce_delaunay_property_using_edge_flips(newV, iNewVert, triStack);
            conform_to_edge(edge_t(iA, iNewVert), originalEdges, overlaps);
            conform_to_edge(edge_t(iNewVert, iB), originalEdges, overlaps);
            return;
        }

        iT = iTopo;
        t = triangles[iT];

        const point_to_line_location_t::Enum loc = locate_point_wrt_line(vOpo, a, b, distanceTolerance);
        if (loc == point_to_line_location_t::LEFT_SIDE) {
            iV = iVleft;
            iVleft = iVopo;
        } else if (loc == point_to_line_location_t::RIGHT_SIDE) {
            iV = iVright;
            iVright = iVopo;
        } else // encountered point on the edge
            iB = iVopo;
    }
    /**/

    // add mid-point to triangulation
    const std::uint32_t iMid = static_cast<std::uint32_t>(vertices.size());
    const vec2_<T>& start = vertices[iA];
    const vec2_<T>& end = vertices[iB];
    create_vertex(
        vec2_<T>::make((start.x() + end.x()) / T(2), (start.y() + end.y()) / T(2)),
        std::vector<std::uint32_t>());
    const std::vector<edge_t> flippedFixedEdges = insert_vertex_and_flip_fixed_edges(iMid);

    conform_to_edge(edge_t(iA, iMid), originalEdges, overlaps);
    conform_to_edge(edge_t(iMid, iB), originalEdges, overlaps);
    // re-introduce fixed edges that were flipped
    // and make sure overlap count is preserved
    for (std::vector<edge_t>::const_iterator it = flippedFixedEdges.begin();
         it != flippedFixedEdges.end();
         ++it) {
        fixedEdges.erase(*it);

        boundary_overlap_count_t prevOverlaps = 0;
        const std::unordered_map<edge_t, boundary_overlap_count_t>::const_iterator
            overlapsIt
            = overlapCount.find(*it);
        if (overlapsIt != overlapCount.end()) {
            prevOverlaps = overlapsIt->second;
            overlapCount.erase(overlapsIt);
        }
        // override overlapping boundaries count when re-inserting an edge
        std::vector<edge_t> prevOriginals(1, *it);
        const std::unordered_map<edge_t, std::vector<edge_t>>::const_iterator originalsIt = pieceToOriginals.find(*it);
        if (originalsIt != pieceToOriginals.end()) {
            prevOriginals = originalsIt->second;
        }
        conform_to_edge(*it, prevOriginals, prevOverlaps);
    }
    if (iB != edge.v2())
        conform_to_edge(edge_t(iB, edge.v2()), originalEdges, overlaps);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::add_super_triangle(const box2d_t<T>& box)
{
    m_nTargetVerts = 3;
    m_superGeomType = super_geometry_type_t::SUPER_TRIANGLE;

    const vec2_<T> center = {
        (box.min.x() + box.max.x()) / T(2), (box.min.y() + box.max.y()) / T(2)
    };
    const T w = box.max.x() - box.min.x();
    const T h = box.max.y() - box.min.y();
    T r = std::sqrt(w * w + h * h) / T(2); // incircle radius
    r *= T(1.1);
    const T R = T(2) * r; // excircle radius
    const T shiftX = R * std::sqrt(T(3)) / T(2); // R * cos(30 deg)
    const vec2_<T> posV1 = { center.x() - shiftX, center.y() - r };
    const vec2_<T> posV2 = { center.x() + shiftX, center.y() - r };
    const vec2_<T> posV3 = { center.x(), center.y() + R };
    create_vertex(posV1, std::vector<std::uint32_t>(1, std::uint32_t(0)));
    create_vertex(posV2, std::vector<std::uint32_t>(1, std::uint32_t(0)));
    create_vertex(posV3, std::vector<std::uint32_t>(1, std::uint32_t(0)));
    const triangle_t superTri = {
        { std::uint32_t(0), std::uint32_t(1), std::uint32_t(2) },
        { null_neighbour, null_neighbour, null_neighbour }
    };
    add_triangle(superTri);
    m_nearPtLocator.initialize(vertices);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::create_vertex(
    const vec2_<T>& pos,
    const std::vector<std::uint32_t>& tris)
{
    vertices.push_back(pos);
    vertTris.push_back(tris);
}

template <typename T, typename TNearPointLocator>
std::vector<edge_t>
triangulator_t<T, TNearPointLocator>::insert_vertex_and_flip_fixed_edges(
    const std::uint32_t iVert)
{
    std::vector<edge_t> flippedFixedEdges;

    const vec2_<T>& v = vertices[iVert];
    std::array<std::uint32_t, 2> trisAt = walking_search_triangle_at(v);
    std::stack<std::uint32_t> triStack = trisAt[1] == null_neighbour
        ? insert_point_in_triangle(iVert, trisAt[0])
        : insert_point_on_edge(iVert, trisAt[0], trisAt[1]);
    while (!triStack.empty()) {
        const std::uint32_t iT = triStack.top();
        triStack.pop();

        const triangle_t& t = triangles[iT];
        const std::uint32_t iTopo = get_opposite_triangle_index(t, iVert);
        if (iTopo == null_neighbour)
            continue;

        /*
         *                       v3         original edge: (v1, v3)
         *                      /|\   flip-candidate edge: (v,  v2)
         *                    /  |  \
         *                  /    |    \
         *                /      |      \
         * new vertex--> v       |       v2
         *                \      |      /
         *                  \    |    /
         *                    \  |  /
         *                      \|/
         *                       v1
         */
        const triangle_t& tOpo = triangles[iTopo];
        const std::uint32_t i = get_opposite_vertex_index(tOpo, iT);
        const std::uint32_t iV2 = tOpo.vertices[i];
        const std::uint32_t iV1 = tOpo.vertices[cw(i)];
        const std::uint32_t iV3 = tOpo.vertices[ccw(i)];

        if (check_is_edgeflip_needed(v, iVert, iV1, iV2, iV3)) {
            // if flipped edge is fixed, remember it
            const edge_t flippedEdge(iV1, iV3);
            if (fixedEdges.count(flippedEdge))
                flippedFixedEdges.push_back(flippedEdge);

            do_edgeflip(iT, iTopo);
            triStack.push(iT);
            triStack.push(iTopo);
        }
    }

    m_nearPtLocator.add_point(iVert, vertices);
    return flippedFixedEdges;
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::insert_vertex(const std::uint32_t iVert)
{
    const vec2_<T>& v = vertices[iVert];

    std::array<std::uint32_t, 2> trisAt = walking_search_triangle_at(v);

    std::stack<std::uint32_t> triStack = (trisAt[1] == null_neighbour)
        ? insert_point_in_triangle(iVert, trisAt[0])
        : insert_point_on_edge(iVert, trisAt[0], trisAt[1]);

    enforce_delaunay_property_using_edge_flips(v, iVert, triStack);

    m_nearPtLocator.add_point(iVert, vertices);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::enforce_delaunay_property_using_edge_flips(
    const vec2_<T>& v,
    const std::uint32_t iVert,
    std::stack<std::uint32_t>& triStack)
{
    while (!triStack.empty()) {

        const std::uint32_t iT = triStack.top();
        triStack.pop();

        const triangle_t& t = triangles[iT];
        const std::uint32_t iTopo = get_opposite_triangle_index(t, iVert);

        if (iTopo == null_neighbour) {
            continue;
        }

        if (check_is_edgeflip_needed(v, iT, iTopo, iVert)) {

            do_edgeflip(iT, iTopo);

            triStack.push(iT);
            triStack.push(iTopo);
        }
    }
}

/*!
 * Handles super-triangle vertices.
 * Super-tri points are not infinitely far and influence the input points
 * Three cases are possible:
 *  1.  If one of the opposed vertices is super-tri: no flip needed
 *  2.  One of the shared vertices is super-tri:
 *      check if on point is same side of line formed by non-super-tri
 * vertices as the non-super-tri shared vertex
 *  3.  None of the vertices are super-tri: normal circumcircle test
 */
/*
 *                       v3         original edge: (v1, v3)
 *                      /|\   flip-candidate edge: (v,  v2)
 *                    /  |  \
 *                  /    |    \
 *                /      |      \
 * new vertex--> v       |       v2
 *                \      |      /
 *                  \    |    /
 *                    \  |  /
 *                      \|/
 *                       v1
 */
template <typename T, typename TNearPointLocator>
bool triangulator_t<T, TNearPointLocator>::check_is_edgeflip_needed(
    const vec2_<T>& v,
    const std::uint32_t iV,
    const std::uint32_t iV1,
    const std::uint32_t iV2,
    const std::uint32_t iV3) const
{
    const vec2_<T>& v1 = vertices[iV1];
    const vec2_<T>& v2 = vertices[iV2];
    const vec2_<T>& v3 = vertices[iV3];
    if (m_superGeomType == super_geometry_type_t::SUPER_TRIANGLE) {
        // If flip-candidate edge touches super-triangle in-circumference
        // test has to be replaced with orient2d test against the line
        // formed by two non-artificial vertices (that don't belong to
        // super-triangle)
        if (iV < 3) // flip-candidate edge touches super-triangle
        {
            // does original edge also touch super-triangle?
            if (iV1 < 3)
                return locate_point_wrt_line(v1, v2, v3) == locate_point_wrt_line(v, v2, v3);
            if (iV3 < 3)
                return locate_point_wrt_line(v3, v1, v2) == locate_point_wrt_line(v, v1, v2);
            return false; // original edge does not touch super-triangle
        }
        if (iV2 < 3) // flip-candidate edge touches super-triangle
        {
            // does original edge also touch super-triangle?
            if (iV1 < 3)
                return locate_point_wrt_line(v1, v, v3) == locate_point_wrt_line(v2, v, v3);
            if (iV3 < 3)
                return locate_point_wrt_line(v3, v1, v) == locate_point_wrt_line(v2, v1, v);
            return false; // original edge does not touch super-triangle
        }
        // flip-candidate edge does not touch super-triangle
        if (iV1 < 3)
            return locate_point_wrt_line(v1, v2, v3) == locate_point_wrt_line(v, v2, v3);
        if (iV3 < 3)
            return locate_point_wrt_line(v3, v1, v2) == locate_point_wrt_line(v, v1, v2);
    }
    return check_is_in_circumcircle(v, v1, v2, v3);
}

template <typename T, typename TNearPointLocator>
bool triangulator_t<T, TNearPointLocator>::check_is_edgeflip_needed(
    const vec2_<T>& v,
    const std::uint32_t iT,
    const std::uint32_t iTopo,
    const std::uint32_t iV) const
{
    /*
     *                       v3         original edge: (v1, v3)
     *                      /|\   flip-candidate edge: (v,  v2)
     *                    /  |  \
     *                  /    |    \
     *                /      |      \
     * new vertex--> v       |       v2
     *                \      |      /
     *                  \    |    /
     *                    \  |  /
     *                      \|/
     *                       v1
     */
    const triangle_t& tOpo = triangles[iTopo];
    const std::uint32_t i = get_opposite_vertex_index(tOpo, iT);
    const std::uint32_t iV2 = tOpo.vertices[i];
    const std::uint32_t iV1 = tOpo.vertices[cw(i)];
    const std::uint32_t iV3 = tOpo.vertices[ccw(i)];

    // flip not needed if the original edge is fixed
    if (fixedEdges.count(edge_t(iV1, iV3)))
        return false;

    return check_is_edgeflip_needed(v, iV, iV1, iV2, iV3);
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::walk_triangles(
    const std::uint32_t startVertex,
    const vec2_<T>& pos) const
{
    // begin walk in search of triangle at pos
    std::uint32_t currTri = vertTris[startVertex][0];

    std::unordered_set<std::uint32_t> visited;

    bool found = false;
    while (!found) {

        const triangle_t& t = triangles[currTri];
        found = true;
        // stochastic offset to randomize which edge we check first
        const std::uint32_t offset(detail::randGenerator() % 3);

        for (std::uint32_t i_(0); i_ < std::uint32_t(3); ++i_) {

            const std::uint32_t i((i_ + offset) % 3);
            const vec2_<T>& vStart = vertices[t.vertices[i]];
            const vec2_<T>& vEnd = vertices[t.vertices[ccw(i)]];
            const point_to_line_location_t::Enum edgeCheck = locate_point_wrt_line(pos, vStart, vEnd);

            if (edgeCheck == point_to_line_location_t::RIGHT_SIDE && t.neighbors[i] != null_neighbour && visited.insert(t.neighbors[i]).second) {
                found = false;
                currTri = t.neighbors[i];
                break;
            }
        }
    }
    return currTri;
}

/* Flip edge between T and Topo:
 *
 *                v4         | - old edge
 *               /|\         ~ - new edge
 *              / | \
 *          n3 /  T' \ n4
 *            /   |   \
 *           /    |    \
 *     T -> v1~~~~~~~~~v3 <- Topo
 *           \    |    /
 *            \   |   /
 *          n1 \Topo'/ n2
 *              \ | /
 *               \|/
 *                v2
 */
template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::do_edgeflip(
    const std::uint32_t iT,
    const std::uint32_t iTopo)
{
    triangle_t& t = triangles[iT];
    triangle_t& tOpo = triangles[iTopo];

    const std::array<std::uint32_t, 3>& triNs = t.neighbors;
    const std::array<std::uint32_t, 3>& triOpoNs = tOpo.neighbors;
    const std::array<std::uint32_t, 3>& triVs = t.vertices;
    const std::array<std::uint32_t, 3>& triOpoVs = tOpo.vertices;

    // find vertices and neighbors
    std::uint32_t i = get_opposite_vertex_index(t, iTopo);

    const std::uint32_t v1 = triVs[i];
    const std::uint32_t v2 = triVs[ccw(i)];
    const std::uint32_t n1 = triNs[i];
    const std::uint32_t n3 = triNs[cw(i)];

    i = get_opposite_vertex_index(tOpo, iT);

    const std::uint32_t v3 = triOpoVs[i];
    const std::uint32_t v4 = triOpoVs[ccw(i)];
    const std::uint32_t n4 = triOpoNs[i];
    const std::uint32_t n2 = triOpoNs[cw(i)];

    // change vertices and neighbors
    using detail::arr3;

    t = triangle_t::make(arr3(v4, v1, v3), arr3(n3, iTopo, n4));
    tOpo = triangle_t::make(arr3(v2, v3, v1), arr3(n2, iT, n1));

    // adjust neighboring triangles and vertices
    change_neighbour(n1, iT, iTopo);
    change_neighbour(n4, iTopo, iT);

    // only adjust adjacent triangles if triangulation is not finalized:
    // can happen when called from outside on an already finalized triangulation
    if (!is_finalized()) {

        add_adjacent_triangle(v1, iTopo);
        add_adjacent_triangle(v3, iT);

        remove_adjacent_triangle(v2, iT);
        remove_adjacent_triangle(v4, iTopo);
    }
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::change_neighbour(
    const std::uint32_t iT,
    const std::uint32_t oldNeighbor,
    const std::uint32_t newNeighbor)
{
    if (iT == null_neighbour) {
        return;
    }

    triangle_t& t = triangles[iT];

    t.neighbors[get_neighbour_index(t, oldNeighbor)] = newNeighbor;
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::add_adjacent_triangle(
    const std::uint32_t iVertex,
    const std::uint32_t iTriangle)
{
    vertTris[iVertex].push_back(iTriangle);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::add_adjacent_triangles(
    const std::uint32_t iVertex,
    const std::uint32_t iT1,
    const std::uint32_t iT2,
    const std::uint32_t iT3)
{
    std::vector<std::uint32_t>& vTris = vertTris[iVertex];

    vTris.reserve(vTris.size() + 3);

    vTris.push_back(iT1);
    vTris.push_back(iT2);
    vTris.push_back(iT3);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::add_adjacent_triangles(
    const std::uint32_t iVertex,
    const std::uint32_t iT1,
    const std::uint32_t iT2,
    const std::uint32_t iT3,
    const std::uint32_t iT4)
{
    std::vector<std::uint32_t>& vTris = vertTris[iVertex];

    vTris.reserve(vTris.size() + 4);

    vTris.push_back(iT1);
    vTris.push_back(iT2);
    vTris.push_back(iT3);
    vTris.push_back(iT4);
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::remove_adjacent_triangle(
    const std::uint32_t iVertex,
    const std::uint32_t iTriangle)
{
    std::vector<std::uint32_t>& tris = vertTris[iVertex];
    tris.erase(std::find(tris.begin(), tris.end(), iTriangle));
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::triangulate_pseudo_polygon(
    const std::uint32_t ia,
    const std::uint32_t ib,
    const std::vector<std::uint32_t>::const_iterator pointsFirst,
    const std::vector<std::uint32_t>::const_iterator pointsLast)
{
    if (pointsFirst == pointsLast)
        return pseudo_polygon_outer_triangle(ia, ib);

    // Find delaunay point
    const std::uint32_t ic = find_delaunay_point(ia, ib, pointsFirst, pointsLast);
    // Find pseudopolygons split by the delaunay point
    std::vector<std::uint32_t>::const_iterator newLast = pointsFirst;

    while (*newLast != ic) {
        ++newLast;
    }

    const std::vector<std::uint32_t>::const_iterator newFirst = newLast + 1;
    // triangulate splitted pseudo-polygons
    const std::uint32_t iT2 = triangulate_pseudo_polygon(ic, ib, newFirst, pointsLast);
    const std::uint32_t iT1 = triangulate_pseudo_polygon(ia, ic, pointsFirst, newLast);
    // add new triangle
    const triangle_t t = { { ia, ib, ic }, { null_neighbour, iT2, iT1 } };
    const std::uint32_t iT = add_triangle(t);

    // adjust neighboring triangles and vertices
    if (iT1 != null_neighbour) {
        if (pointsFirst == newLast) {
            change_neighbour(iT1, ia, ic, iT);
        } else {
            triangles[iT1].neighbors[0] = iT;
        }
    }
    if (iT2 != null_neighbour) {
        if (newFirst == pointsLast) {
            change_neighbour(iT2, ic, ib, iT);
        } else {
            triangles[iT2].neighbors[0] = iT;
        }
    }

    add_adjacent_triangle(ia, iT);
    add_adjacent_triangle(ib, iT);
    add_adjacent_triangle(ic, iT);

    return iT;
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::find_delaunay_point(
    const std::uint32_t ia,
    const std::uint32_t ib,
    const std::vector<std::uint32_t>::const_iterator pointsFirst,
    const std::vector<std::uint32_t>::const_iterator pointsLast) const
{
    MCUT_ASSERT(pointsFirst != pointsLast);

    const vec2_<T>& a = vertices[ia];
    const vec2_<T>& b = vertices[ib];

    std::uint32_t ic = *pointsFirst;
    vec2_<T> c = vertices[ic];

    for (std::vector<std::uint32_t>::const_iterator it = pointsFirst + 1; it != pointsLast; ++it) {

        const vec2_<T> v = vertices[*it];

        if (!check_is_in_circumcircle(v, a, b, c))
        {
            continue;
        }

        ic = *it;
        c = vertices[ic];
    }
    return ic;
}

template <typename T, typename TNearPointLocator>
std::uint32_t triangulator_t<T, TNearPointLocator>::pseudo_polygon_outer_triangle(
    const std::uint32_t ia,
    const std::uint32_t ib) const
{

    const std::vector<std::uint32_t>& aTris = vertTris[ia];
    const std::vector<std::uint32_t>& bTris = vertTris[ib];

    for (std::vector<std::uint32_t>::const_iterator it = aTris.begin(); it != aTris.end(); ++it)
    {
        if (std::find(bTris.begin(), bTris.end(), *it) != bTris.end())
        {
            return *it;
        }
    }

    return null_neighbour;
}

template <typename T, typename TNearPointLocator>
void triangulator_t<T, TNearPointLocator>::insert_vertices(
    const std::vector<vec2_<T>>& newVertices)
{
    return insert_vertices(
        newVertices.begin(), newVertices.end(), get_x_coord_vec2d<T>, get_y_coord_vec2d<T>);
}

template <typename T, typename TNearPointLocator>
bool triangulator_t<T, TNearPointLocator>::is_finalized() const
{
    return vertTris.empty() && !vertices.empty();
}

} // namespace cdt
#endif

#endif // header-guard
