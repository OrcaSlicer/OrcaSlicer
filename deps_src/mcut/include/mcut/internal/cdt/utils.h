/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef _CDT_UTILITIES_H_
#define _CDT_UTILITIES_H_

#include "mcut/internal/math.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

#include <array>
#include <functional>
#include <random>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace cdt {

/// X- coordinate getter for vec2d_t
template <typename T>
const T& get_x_coord_vec2d(const vec2_<T>& v)
{
    return v.x();
}

/// Y-coordinate getter for vec2d_t
template <typename T>
const T& get_y_coord_vec2d(const vec2_<T>& v)
{
    return v.y();
}

/// If two 2D vectors are exactly equal
template <typename T>
bool operator==(const vec2_<T>& lhs, const vec2_<T>& rhs)
{
    return lhs.x() == rhs.x() && lhs.y() == rhs.y();
}

/// Constant representing no valid neighbor for a triangle
const static std::uint32_t null_neighbour(std::numeric_limits<std::uint32_t>::max());
/// Constant representing no valid vertex for a triangle
const static std::uint32_t null_vertex(std::numeric_limits<std::uint32_t>::max());

/// 2D bounding box
template <typename T>
struct box2d_t {
    vec2_<T> min; ///< min box corner
    vec2_<T> max; ///< max box corner

    /// Envelop box around a point
    void expand_with_point(const vec2_<T>& p)
    {
        expand_with_point(p.x(), p.y());
    }
    /// Envelop box around a point with given coordinates
    void expand_with_point(const T x, const T y)
    {
        min.x() = std::min(x, min.x());
        max.x() = std::max(x, max.x());
        min.y() = std::min(y, min.y());
        max.y() = std::max(y, max.y());
    }
};

/// Bounding box of a collection of custom 2D points given coordinate getters
template <
    typename T,
    typename TVertexIter,
    typename TGetVertexCoordX,
    typename TGetVertexCoordY>
box2d_t<T> expand_with_points(
    TVertexIter first,
    TVertexIter last,
    TGetVertexCoordX get_x_coord,
    TGetVertexCoordY get_y_coord)
{
    const T max = std::numeric_limits<T>::max();
    box2d_t<T> box = { { max, max }, { -max, -max } };

    for (; first != last; ++first) {
        box.expand_with_point(get_x_coord(*first), get_y_coord(*first));
    }
    return box;
}

/// Bounding box of a collection of 2D points
template <typename T>
box2d_t<T> expand_with_points(const std::vector<vec2_<T>>& vertices);

/// edge_t connecting two vertices: vertex with smaller index is always first
/// \note: hash edge_t is specialized at the bottom
struct edge_t {
    
    edge_t(std::uint32_t iV1, std::uint32_t iV2)
        : m_vertices(
            iV1 < iV2 ? std::make_pair(iV1, iV2) : std::make_pair(iV2, iV1))
    {
    }

    inline bool operator==(const edge_t& other) const
    {
        return m_vertices == other.m_vertices;
    }

    inline bool operator!=(const edge_t& other) const
    {
        return !(this->operator==(other));
    }

    inline std::uint32_t v1() const
    {
        return m_vertices.first;
    }

    inline std::uint32_t v2() const
    {
        return m_vertices.second;
    }

    inline const std::pair<std::uint32_t, std::uint32_t>& verts() const
    {
        return m_vertices;
    }

private:
    std::pair<std::uint32_t, std::uint32_t> m_vertices;
};

/// Get edge first vertex
inline std::uint32_t edge_get_v1(const edge_t& e)
{
    return e.v1();
}

/// Get edge second vertex
inline std::uint32_t edge_get_v2(const edge_t& e)
{
    return e.v2();
}

/// Get edge second vertex
inline edge_t edge_make(std::uint32_t iV1, std::uint32_t iV2)
{
    return edge_t(iV1, iV2);
}

/// triangulator_t triangle (CCW winding)
/* Counter-clockwise winding:
       v3
       /\
    n3/  \n2
     /____\
   v1  n1  v2                 */
struct triangle_t {

    std::array<std::uint32_t, 3> vertices; 
    std::array<std::uint32_t, 3> neighbors; 

    /**
     * Factory method
     * @note needed for c++03 compatibility (no uniform initialization
     * available)
     */
    static triangle_t
    make(const std::array<std::uint32_t, 3>& vertices, const std::array<std::uint32_t, 3>& neighbors)
    {
        triangle_t t = { vertices, neighbors };
        return t;
    }
};

/// Location of point on a triangle
struct point_to_triangle_location_t {
    /// Enum
    enum Enum {
        INSIDE,
        OUTSIDE,
        ON_1ST_EDGE,
        ON_2ND_EDGE,
        ON_3RD_EDGE,
    };
};

/// Relative location of point to a line
struct point_to_line_location_t {
    /// Enum
    enum Enum {
        LEFT_SIDE,
        RIGHT_SIDE,
        COLLINEAR,
    };
};

} // namespace cdt

#ifndef CDT_USE_AS_COMPILED_LIBRARY

#include <stdexcept>

namespace cdt {

//*****************************************************************************
// box2d_t
//*****************************************************************************
template <typename T>
box2d_t<T> expand_with_points(const std::vector<vec2_<T>>& vertices)
{
    return expand_with_points<T>(
        vertices.begin(), vertices.end(), get_x_coord_vec2d<T>, get_y_coord_vec2d<T>);
}

//*****************************************************************************
// Utility functions
//*****************************************************************************

/// Advance vertex or neighbor index counter-clockwise
inline std::uint32_t ccw(std::uint32_t i)
{
    return std::uint32_t((i + 1) % 3);
}

/// Advance vertex or neighbor index clockwise
inline std::uint32_t cw(std::uint32_t i)
{
    return std::uint32_t((i + 2) % 3);
}

/// Check if location is classified as on any of three edges
inline bool check_on_edge(const point_to_triangle_location_t::Enum location)
{
    return location > point_to_triangle_location_t::OUTSIDE;
}

/// Neighbor index from a on-edge location
/// \note Call only if located on the edge!
inline std::uint32_t edge_neighbour(const point_to_triangle_location_t::Enum location)
{
    assert(location >= point_to_triangle_location_t::ON_1ST_EDGE);
    return static_cast<std::uint32_t>(location - point_to_triangle_location_t::ON_1ST_EDGE);
}

#if 0
/// Orient p against line v1-v2 2D: robust geometric predicate
template <typename T>
T orient2D(const vec2_<T>& p, const vec2_<T>& v1, const vec2_<T>& v2)
{
    return orient2d(v1.x(), v1.y(), v2.x(), v2.y(), p.x(), p.y());
}
#endif

/// Classify value of orient2d predicate
template <typename T>
point_to_line_location_t::Enum
classify_orientation(const T orientation, const T orientationTolerance = T(0))
{
    if (orientation < -orientationTolerance)
        return point_to_line_location_t::RIGHT_SIDE;
    if (orientation > orientationTolerance)
        return point_to_line_location_t::LEFT_SIDE;
    return point_to_line_location_t::COLLINEAR;
}

/// Check if point lies to the left of, to the right of, or on a line
template <typename T>
point_to_line_location_t::Enum locate_point_wrt_line(
    const vec2_<T>& p,
    const vec2_<T>& v1,
    const vec2_<T>& v2,
    const T orientationTolerance = T(0))
{
    return classify_orientation(orient2d(p, v1, v2), orientationTolerance);
}

/// Check if point a lies inside of, outside of, or on an edge of a triangle
template <typename T>
point_to_triangle_location_t::Enum locate_point_wrt_triangle(
    const vec2_<T>& p,
    const vec2_<T>& v1,
    const vec2_<T>& v2,
    const vec2_<T>& v3)
{
    point_to_triangle_location_t::Enum result = point_to_triangle_location_t::INSIDE;
    point_to_line_location_t::Enum edgeCheck = locate_point_wrt_line(p, v1, v2);
    if (edgeCheck == point_to_line_location_t::RIGHT_SIDE)
        return point_to_triangle_location_t::OUTSIDE;
    if (edgeCheck == point_to_line_location_t::COLLINEAR)
        result = point_to_triangle_location_t::ON_1ST_EDGE;
    edgeCheck = locate_point_wrt_line(p, v2, v3);
    if (edgeCheck == point_to_line_location_t::RIGHT_SIDE)
        return point_to_triangle_location_t::OUTSIDE;
    if (edgeCheck == point_to_line_location_t::COLLINEAR)
        result = point_to_triangle_location_t::ON_2ND_EDGE;
    edgeCheck = locate_point_wrt_line(p, v3, v1);
    if (edgeCheck == point_to_line_location_t::RIGHT_SIDE)
        return point_to_triangle_location_t::OUTSIDE;
    if (edgeCheck == point_to_line_location_t::COLLINEAR)
        result = point_to_triangle_location_t::ON_3RD_EDGE;
    return result;
}

/// Opposed neighbor index from vertex index
inline std::uint32_t get_opposite_neighbour_from_vertex(const std::uint32_t vertIndex)
{
    MCUT_ASSERT(vertIndex < 3);

    if (vertIndex == std::uint32_t(0))
        return std::uint32_t(1);
    if (vertIndex == std::uint32_t(1))
        return std::uint32_t(2);
    if (vertIndex == std::uint32_t(2))
        return std::uint32_t(0);
    throw std::runtime_error("Invalid vertex index");
}
/// Opposed vertex index from neighbor index
inline std::uint32_t opposite_vertex_from_neighbour(const std::uint32_t neighborIndex)
{
    if (neighborIndex == std::uint32_t(0))
        return std::uint32_t(2);
    if (neighborIndex == std::uint32_t(1))
        return std::uint32_t(0);
    if (neighborIndex == std::uint32_t(2))
        return std::uint32_t(1);
    throw std::runtime_error("Invalid neighbor index");
}

/// Index of triangle's neighbor opposed to a vertex
inline std::uint32_t
opposite_triangle_index(const triangle_t& tri, const std::uint32_t iVert)
{
    for (std::uint32_t vi = std::uint32_t(0); vi < std::uint32_t(3); ++vi)
        if (iVert == tri.vertices[vi])
            return get_opposite_neighbour_from_vertex(vi);
    throw std::runtime_error("Could not find opposed triangle index");
}

/// Index of triangle's neighbor opposed to an edge
inline std::uint32_t opposite_triangle_index(
    const triangle_t& tri,
    const std::uint32_t iVedge1,
    const std::uint32_t iVedge2)
{
    for (std::uint32_t vi = std::uint32_t(0); vi < std::uint32_t(3); ++vi) {
        const std::uint32_t iVert = tri.vertices[vi];
        if (iVert != iVedge1 && iVert != iVedge2)
            return get_opposite_neighbour_from_vertex(vi);
    }
    throw std::runtime_error("Could not find opposed-to-edge triangle index");
}

/// Index of triangle's vertex opposed to a triangle
inline std::uint32_t
get_opposite_vertex_index(const triangle_t& tri, const std::uint32_t iTopo)
{
    for (std::uint32_t ni = std::uint32_t(0); ni < std::uint32_t(3); ++ni)
        if (iTopo == tri.neighbors[ni])
            return opposite_vertex_from_neighbour(ni);
    throw std::runtime_error("Could not find opposed vertex index");
}

/// If triangle has a given neighbor return neighbor-index, throw otherwise
inline std::uint32_t
get_neighbour_index(const triangle_t& tri, std::uint32_t iTnbr)
{
    for (std::uint32_t ni = std::uint32_t(0); ni < std::uint32_t(3); ++ni)
        if (iTnbr == tri.neighbors[ni])
            return ni;
    throw std::runtime_error("Could not find neighbor triangle index");
}

/// If triangle has a given vertex return vertex-index, throw otherwise
inline std::uint32_t get_vertex_index(const triangle_t& tri, const std::uint32_t iV)
{
    for (std::uint32_t i = std::uint32_t(0); i < std::uint32_t(3); ++i)
        if (iV == tri.vertices[i])
            return i;
    throw std::runtime_error("Could not find vertex index in triangle");
}

/// Given triangle and a vertex find opposed triangle
inline std::uint32_t
get_opposite_triangle_index(const triangle_t& tri, const std::uint32_t iVert)
{
    return tri.neighbors[opposite_triangle_index(tri, iVert)];
}

/// Given two triangles, return vertex of first triangle opposed to the second
inline std::uint32_t
get_opposed_vertex_index(const triangle_t& tri, std::uint32_t iTopo)
{
    return tri.vertices[get_opposite_vertex_index(tri, iTopo)];
}

/// Test if point lies in a circumscribed circle of a triangle
template <typename T>
bool check_is_in_circumcircle(
    const vec2_<T>& p,
    const vec2_<T>& v1,
    const vec2_<T>& v2,
    const vec2_<T>& v3)
{
    const double p_[2] = { static_cast<double>(p.x()), static_cast<double>(p.y()) };
    const double v1_[2] = { static_cast<double>(v1.x()), static_cast<double>(v1.y()) };
    const double v2_[2] = { static_cast<double>(v2.x()), static_cast<double>(v2.y()) };
    const double v3_[2] = { static_cast<double>(v3.x()), static_cast<double>(v3.y()) };

    return ::incircle(v1_, v2_, v3_, p_) > T(0);
}

/// Test if two vertices share at least one common triangle
inline bool check_vertices_share_edge(const std::vector<std::uint32_t>& aTris, const std::vector<std::uint32_t>& bTris)
{
    for (std::vector<std::uint32_t>::const_iterator it = aTris.begin(); it != aTris.end(); ++it)
        if (std::find(bTris.begin(), bTris.end(), *it) != bTris.end())
            return true;
    return false;
}

template <typename T>
T get_square_distance(const T ax, const T ay, const T bx, const T by)
{
    const T dx = bx - ax;
    const T dy = by - ay;
    return dx * dx + dy * dy;
}

template <typename T>
T distance(const T ax, const T ay, const T bx, const T by)
{
    return std::sqrt(get_square_distance(ax, ay, bx, by));
}

template <typename T>
T distance(const vec2_<T>& a, const vec2_<T>& b)
{
    return distance(a.x(), a.y(), b.x(), b.y());
}

template <typename T>
T get_square_distance(const vec2_<T>& a, const vec2_<T>& b)
{
    return get_square_distance(a.x(), a.y(), b.x(), b.y());
}

} // namespace cdt

#endif

//*****************************************************************************
// Specialize hash functions
//*****************************************************************************
namespace std {
/// edge_t hasher
template <>
struct hash<cdt::edge_t> {
    /// Hash operator
    std::size_t operator()(const cdt::edge_t& e) const
    {
        return get_hashed_edge_index(e);
    }

private:
    static void combine_hash_values(std::size_t& seed, const std::uint32_t& key)
    {
        seed ^= std::hash<std::uint32_t>()(key) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    static std::size_t get_hashed_edge_index(const cdt::edge_t& e)
    {
        const std::pair<std::uint32_t, std::uint32_t>& vv = e.verts();
        std::size_t seed1(0);
        combine_hash_values(seed1, vv.first);
        combine_hash_values(seed1, vv.second);
        std::size_t seed2(0);
        combine_hash_values(seed2, vv.second);
        combine_hash_values(seed2, vv.first);
        return std::min(seed1, seed2);
    }
};
} // namespace std/boost

#endif // header guard
