#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "Tesselate.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "Utils.hpp"
// BBS
#include "MeshBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <queue>
#include <mutex>
#include <utility>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>

#ifndef NDEBUG
//    #define EXPENSIVE_DEBUG_CHECKS
#endif // NDEBUG

#if 0
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
    #define SLIC3R_DEBUG
// #define SLIC3R_TRIANGLEMESH_DEBUG
#endif

#include <assert.h>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>

// #define SLIC3R_DEBUG_SLICE_PROCESSING

#if defined(SLIC3R_DEBUG) || defined(SLIC3R_DEBUG_SLICE_PROCESSING)
#include "SVG.hpp"
#endif

namespace Slic3r {
const float epson = 1e-3;
bool is_equal(float lh, float rh)
{
    return abs(lh - rh) <= epson;
}

bool is_equal_for_sort(float lh, float rh) {
    return abs(lh - rh) <= 1e-8;
}

bool is_equal(const Vec3f &lh, const Vec3f &rh) { 
    return is_equal(lh[0], rh[0]) && is_equal(lh[1], rh[1]) && is_equal(lh[2], rh[2]);
}

class IntersectionReference
{
public:
    IntersectionReference() = default;
    IntersectionReference(int point_id, int edge_id) : point_id(point_id), edge_id(edge_id) {}
    // Where is this intersection point located? On mesh vertex or mesh edge?
    // Only one of the following will be set, the other will remain set to -1.
    // Index of the mesh vertex.
    int point_id { -1 };
    // Index of the mesh edge.
    int edge_id { -1 };
};

class IntersectionPoint : public Point, public IntersectionReference
{
public:
    IntersectionPoint() = default;
    IntersectionPoint(int point_id, int edge_id, const Point &pt) : IntersectionReference(point_id, edge_id), Point(pt) {}
    IntersectionPoint(const IntersectionReference &ir, const Point &pt) : IntersectionReference(ir), Point(pt) {}
    // Inherits coord_t x, y
};

class IntersectionLine : public Line
{
public:
    IntersectionLine() = default;

    bool skip() const { return (this->flags & SKIP) != 0; }
    void set_skip() { this->flags |= SKIP; }

    bool is_seed_candidate() const { return (this->flags & NO_SEED) == 0 && ! this->skip(); }
    void set_no_seed(bool set) { if (set) this->flags |= NO_SEED; else this->flags &= ~NO_SEED; }

    void reverse() { std::swap(a, b); std::swap(a_id, b_id); std::swap(edge_a_id, edge_b_id); }
    
    // Inherits Point a, b
    // For each line end point, either {a,b}_id or {a,b}edge_a_id is set, the other is left to -1.
    // Vertex indices of the line end points.
    int             a_id { -1 };
    int             b_id { -1 };
    // Source mesh edges of the line end points.
    int             edge_a_id { -1 };
    int             edge_b_id { -1 };

    enum class FacetEdgeType { 
        // A general case, the cutting plane intersect a face at two different edges.
        General,
        // Two vertices are aligned with the cutting plane, the third vertex is below the cutting plane.
        Top,
        // Two vertices are aligned with the cutting plane, the third vertex is above the cutting plane.
        Bottom,
        // Two vertices are aligned with the cutting plane, the edge is shared by two triangles, where one
        // triangle is below or at the cutting plane and the other is above or at the cutting plane (only one
        // vertex may lie on the plane).
        TopBottom,
        // All three vertices of a face are aligned with the cutting plane.
        Horizontal,
        // Edge 
        Slab,
    };

    // feGeneral, feTop, feBottom, feHorizontal
    FacetEdgeType   edge_type { FacetEdgeType::General };
    // Used to skip duplicate edges.
    enum {
        // Triangle edge added, because it has no neighbor.
        EDGE0_NO_NEIGHBOR   = 0x001,
        EDGE1_NO_NEIGHBOR   = 0x002,
        EDGE2_NO_NEIGHBOR   = 0x004,
        // Triangle edge added, because it makes a fold with another horizontal edge.
        EDGE0_FOLD          = 0x010,
        EDGE1_FOLD          = 0x020,
        EDGE2_FOLD          = 0x040,
        // The edge cannot be a seed of a greedy loop extraction (folds are not safe to become seeds).
        NO_SEED             = 0x100,
        SKIP                = 0x200,
    };
    uint32_t        flags { 0 };

#ifndef NDEBUG
    enum class Source {
        BottomPlane,
        TopPlane,
        Slab,
    };
    Source          source { Source::BottomPlane };
#endif // NDEBUG
};

using IntersectionLines = std::vector<IntersectionLine>;

enum class FacetSliceType {
    NoSlice = 0,
    Slicing = 1,
    Cutting = 2
};

// Return true, if the facet has been sliced and line_out has been filled.
static FacetSliceType slice_facet(
    // Z height of the slice in XY plane. Scaled or unscaled (same as vertices[].z()).
    float                                slice_z,
    // 3 vertices of the triangle, XY scaled. Z scaled or unscaled (same as slice_z).
    const stl_vertex                    *vertices,
    const stl_triangle_vertex_indices   &indices,
    const Vec3i32                         &edge_ids,
    const int                            idx_vertex_lowest,
    const bool                           horizontal,
    IntersectionLine                    &line_out)
{
    IntersectionPoint points[3];
    size_t            num_points = 0;
    auto              point_on_layer = size_t(-1);

    // Reorder vertices so that the first one is the one with lowest Z.
    // This is needed to get all intersection lines in a consistent order
    // (external on the right of the line)
    for (int j = 0; j < 3; ++ j) {  // loop through facet edges
        int               edge_id;
        const stl_vertex *a, *b, *c;
        int               a_id, b_id;
        {
            int   k = (idx_vertex_lowest + j) % 3;
            int   l = (k + 1) % 3;
            edge_id = edge_ids(k);
            a_id    = indices[k];
            a       = vertices + k;
            b_id    = indices[l];
            b       = vertices + l;
            c       = vertices + (k + 2) % 3;
        }

        // Is edge or face aligned with the cutting plane?
        if (a->z() == slice_z && b->z() == slice_z) {
            // Edge is horizontal and belongs to the current layer.
            // The following rotation of the three vertices may not be efficient, but this branch happens rarely.
            const stl_vertex &v0 = vertices[0];
            const stl_vertex &v1 = vertices[1];
            const stl_vertex &v2 = vertices[2];
            // We may ignore this edge for slicing purposes, but we may still use it for object cutting.
            FacetSliceType    result = FacetSliceType::Slicing;
            if (horizontal) {
                // All three vertices are aligned with slice_z.
                line_out.edge_type = IntersectionLine::FacetEdgeType::Horizontal;
                result = FacetSliceType::Cutting;
                double normal = (v1.x() - v0.x()) * (v2.y() - v1.y()) - (v1.y() - v0.y()) * (v2.x() - v1.x());
                if (normal < 0) {
                    // If normal points downwards this is a bottom horizontal facet so we reverse its point order.
                    std::swap(a, b);
                    std::swap(a_id, b_id);
                }
            } else {
                // Two vertices are aligned with the cutting plane, the third vertex is below or above the cutting plane.
                // Is the third vertex below the cutting plane?
                bool third_below = v0.z() < slice_z || v1.z() < slice_z || v2.z() < slice_z;
                // Two vertices on the cutting plane, the third vertex is below the plane. Consider the edge to be part of the slice
                // only if it is the upper edge.
                // (the bottom most edge resp. vertex of a triangle is not owned by the triangle, but the top most edge resp. vertex is part of the triangle
                // in respect to the cutting plane).
                result = third_below ? FacetSliceType::Slicing : FacetSliceType::Cutting;
                if (third_below) {
                    line_out.edge_type = IntersectionLine::FacetEdgeType::Top;
                    std::swap(a, b);
                    std::swap(a_id, b_id);
                } else
                    line_out.edge_type = IntersectionLine::FacetEdgeType::Bottom;
            }
            line_out.a.x()  = a->x();
            line_out.a.y()  = a->y();
            line_out.b.x()  = b->x();
            line_out.b.y()  = b->y();
            line_out.a_id   = a_id;
            line_out.b_id   = b_id;
            assert(line_out.a != line_out.b);
            return result;
        }

        if (a->z() == slice_z) {
            // Only point a alings with the cutting plane.
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                point_on_layer = num_points;
                IntersectionPoint &point = points[num_points ++];
                point.x()      = a->x();
                point.y()      = a->y();
                point.point_id = a_id;
            }
        } else if (b->z() == slice_z) {
            // Only point b alings with the cutting plane.
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                point_on_layer = num_points;
                IntersectionPoint &point = points[num_points ++];
                point.x()      = b->x();
                point.y()      = b->y();
                point.point_id = b_id;
            }
        } else if ((a->z() < slice_z && b->z() > slice_z) || (b->z() < slice_z && a->z() > slice_z)) {
            // A general case. The face edge intersects the cutting plane. Calculate the intersection point.
            assert(a_id != b_id);
            // Sort the edge to give a consistent answer.
            if (a_id > b_id) {
                std::swap(a_id, b_id);
                std::swap(a, b);
            }
            IntersectionPoint &point = points[num_points];
            double t = (double(slice_z) - double(b->z())) / (double(a->z()) - double(b->z()));
            if (t <= 0.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                    point.x() = a->x();
                    point.y() = a->y();
                    point_on_layer = num_points ++;
                    point.point_id = a_id;
                }
            } else if (t >= 1.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                    point.x() = b->x();
                    point.y() = b->y();
                    point_on_layer = num_points ++;
                    point.point_id = b_id;
                }
            } else {
                point.x() = coord_t(floor(double(b->x()) + (double(a->x()) - double(b->x())) * t + 0.5));
                point.y() = coord_t(floor(double(b->y()) + (double(a->y()) - double(b->y())) * t + 0.5));
                point.edge_id = edge_id;
                ++ num_points;
            }
        }
    }

    // Facets must intersect each plane 0 or 2 times, or it may touch the plane at a single vertex only.
    assert(num_points < 3);
    if (num_points == 2) {
        line_out.edge_type  = IntersectionLine::FacetEdgeType::General;
        line_out.a          = static_cast<const Point&>(points[1]);
        line_out.b          = static_cast<const Point&>(points[0]);
        line_out.a_id       = points[1].point_id;
        line_out.b_id       = points[0].point_id;
        line_out.edge_a_id  = points[1].edge_id;
        line_out.edge_b_id  = points[0].edge_id;
        // Not a zero lenght edge.
        //FIXME slice_facet() may create zero length edges due to rounding of doubles into coord_t.
        //assert(line_out.a != line_out.b);
        // The plane cuts at least one edge in a general position.
        assert(line_out.a_id == -1 || line_out.b_id == -1);
        assert(line_out.edge_a_id != -1 || line_out.edge_b_id != -1);
        // General slicing position, use the segment for both slicing and object cutting.
#if 0
        if (line_out.a_id != -1 && line_out.b_id != -1) {
            // Solving a degenerate case, where both the intersections snapped to an edge.
            // Correctly classify the face as below or above based on the position of the 3rd point.
            int i = indices[0];
            if (i == line_out.a_id || i == line_out.b_id)
                i = indices[1];
            if (i == line_out.a_id || i == line_out.b_id)
                i = indices[2];
            assert(i != line_out.a_id && i != line_out.b_id);
            line_out.edge_type = ((m_use_quaternion ?
                                    (m_quaternion * this->v_scaled_shared[i]).z()
                                    : this->v_scaled_shared[i].z()) < slice_z) ? IntersectionLine::FacetEdgeType::Top : IntersectionLine::FacetEdgeType::Bottom;
        }
#endif
        return FacetSliceType::Slicing;
    }
    return FacetSliceType::NoSlice;
}

// Return true, if the facet has been sliced and line_out has been filled.
static FacetSliceType slice_facet_for_cut_mesh(
    // Z height of the slice in XY plane. Scaled or unscaled (same as vertices[].z()).
    float slice_z,
    // 3 vertices of the triangle, XY scaled. Z scaled or unscaled (same as slice_z).
    const stl_vertex *                 vertices,
    const stl_triangle_vertex_indices &indices,
    const Vec3i32 &                      edge_ids,
    const int                          idx_vertex_lowest,
    const bool                         horizontal,
    IntersectionLine &                 line_out)
{
    IntersectionPoint points[3];
    size_t            num_points     = 0;
    auto              point_on_layer = size_t(-1);

    // Reorder vertices so that the first one is the one with lowest Z.
    // This is needed to get all intersection lines in a consistent order
    // (external on the right of the line)
    for (int j = 0; j < 3; ++j) { // loop through facet edges
        int               edge_id;
        const stl_vertex *a, *b, *c;
        int               a_id, b_id;
        {
            int k   = (idx_vertex_lowest + j) % 3;
            int l   = (k + 1) % 3;
            edge_id = edge_ids(k);
            a_id    = indices[k];
            a       = vertices + k;
            b_id    = indices[l];
            b       = vertices + l;
            c       = vertices + (k + 2) % 3;
        }

        // Is edge or face aligned with the cutting plane?
        if (is_equal(a->z(), slice_z) && is_equal(b->z(), slice_z)) {
            // Edge is horizontal and belongs to the current layer.
            // The following rotation of the three vertices may not be efficient, but this branch happens rarely.
            const stl_vertex &v0 = vertices[0];
            const stl_vertex &v1 = vertices[1];
            const stl_vertex &v2 = vertices[2];
            // We may ignore this edge for slicing purposes, but we may still use it for object cutting.
            FacetSliceType result = FacetSliceType::Slicing;
            if (horizontal) {
                // All three vertices are aligned with slice_z.
                line_out.edge_type = IntersectionLine::FacetEdgeType::Horizontal;
                result             = FacetSliceType::Cutting;
                double normal      = (v1.x() - v0.x()) * (v2.y() - v1.y()) - (v1.y() - v0.y()) * (v2.x() - v1.x());
                if (normal < 0) {
                    // If normal points downwards this is a bottom horizontal facet so we reverse its point order.
                    std::swap(a, b);
                    std::swap(a_id, b_id);
                }
            } else {
                // Two vertices are aligned with the cutting plane, the third vertex is below or above the cutting plane.
                // Is the third vertex below the cutting plane?
                bool third_below = c->z() < slice_z;
                // Two vertices on the cutting plane, the third vertex is below the plane. Consider the edge to be part of the slice
                // only if it is the upper edge.
                // (the bottom most edge resp. vertex of a triangle is not owned by the triangle, but the top most edge resp. vertex is part of the triangle
                // in respect to the cutting plane).
                result = third_below ? FacetSliceType::Slicing : FacetSliceType::Cutting;
                if (third_below) {
                    line_out.edge_type = IntersectionLine::FacetEdgeType::Top;
                    std::swap(a, b);
                    std::swap(a_id, b_id);
                } else
                    line_out.edge_type = IntersectionLine::FacetEdgeType::Bottom;
            }
            line_out.a.x() = a->x();
            line_out.a.y() = a->y();
            line_out.b.x() = b->x();
            line_out.b.y() = b->y();
            line_out.a_id  = a_id;
            line_out.b_id  = b_id;
            assert(line_out.a != line_out.b);
            return result;
        }

        if (is_equal(a->z(), slice_z)) {
            // Only point a alings with the cutting plane.
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                point_on_layer           = num_points;
                IntersectionPoint &point = points[num_points++];
                point.x()                = a->x();
                point.y()                = a->y();
                point.point_id           = a_id;
            }
        } else if (is_equal(b->z(), slice_z)) {
            // Only point b alings with the cutting plane.
            if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                point_on_layer           = num_points;
                IntersectionPoint &point = points[num_points++];
                point.x()                = b->x();
                point.y()                = b->y();
                point.point_id           = b_id;
            }
        } else if ((a->z() < slice_z && b->z() > slice_z) || (b->z() < slice_z && a->z() > slice_z)) {
            // A general case. The face edge intersects the cutting plane. Calculate the intersection point.
            assert(a_id != b_id);
            // Sort the edge to give a consistent answer.
            if (a_id > b_id) {
                std::swap(a_id, b_id);
                std::swap(a, b);
            }
            IntersectionPoint &point = points[num_points];
            double             t     = (double(slice_z) - double(b->z())) / (double(a->z()) - double(b->z()));
            if (t <= 0.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != a_id) {
                    point.x()      = a->x();
                    point.y()      = a->y();
                    point_on_layer = num_points++;
                    point.point_id = a_id;
                }
            } else if (t >= 1.) {
                if (point_on_layer == size_t(-1) || points[point_on_layer].point_id != b_id) {
                    point.x()      = b->x();
                    point.y()      = b->y();
                    point_on_layer = num_points++;
                    point.point_id = b_id;
                }
            } else {
                point.x()     = coord_t(floor(double(b->x()) + (double(a->x()) - double(b->x())) * t + 0.5));
                point.y()     = coord_t(floor(double(b->y()) + (double(a->y()) - double(b->y())) * t + 0.5));
                point.edge_id = edge_id;
                ++num_points;
            }
        }
    }

    // Facets must intersect each plane 0 or 2 times, or it may touch the plane at a single vertex only.
    assert(num_points < 3);
    if (num_points == 2) {
        line_out.edge_type = IntersectionLine::FacetEdgeType::General;
        line_out.a         = static_cast<const Point &>(points[1]);
        line_out.b         = static_cast<const Point &>(points[0]);
        line_out.a_id      = points[1].point_id;
        line_out.b_id      = points[0].point_id;
        line_out.edge_a_id = points[1].edge_id;
        line_out.edge_b_id = points[0].edge_id;
        // Not a zero lenght edge.
        // FIXME slice_facet() may create zero length edges due to rounding of doubles into coord_t.
        // assert(line_out.a != line_out.b);
        // The plane cuts at least one edge in a general position.
        assert(line_out.a_id == -1 || line_out.b_id == -1);
        assert(line_out.edge_a_id != -1 || line_out.edge_b_id != -1);
        // General slicing position, use the segment for both slicing and object cutting.

        return FacetSliceType::Slicing;
    }
    return FacetSliceType::NoSlice;
}

template<typename TransformVertex>
void slice_facet_at_zs(
    // Scaled or unscaled vertices. transform_vertex_fn may scale zs.
    const std::vector<Vec3f>                         &mesh_vertices,
    const TransformVertex                            &transform_vertex_fn,
    const stl_triangle_vertex_indices                &indices,
    const Vec3i32                                      &edge_ids,
    // Scaled or unscaled zs. If vertices have their zs scaled or transform_vertex_fn scales them, then zs have to be scaled as well.
    const std::vector<float>                         &zs,
    std::vector<IntersectionLines>                   &lines,
    std::array<std::mutex, 64>                       &lines_mutex)
{
    stl_vertex vertices[3] { transform_vertex_fn(mesh_vertices[indices(0)]), transform_vertex_fn(mesh_vertices[indices(1)]), transform_vertex_fn(mesh_vertices[indices(2)]) };

    // find facet extents
    const float min_z = fminf(vertices[0].z(), fminf(vertices[1].z(), vertices[2].z()));
    const float max_z = fmaxf(vertices[0].z(), fmaxf(vertices[1].z(), vertices[2].z()));

    // find layer extents
    auto min_layer = std::lower_bound(zs.begin(), zs.end(), min_z); // first layer whose slice_z is >= min_z
    auto max_layer = std::upper_bound(min_layer, zs.end(), max_z); // first layer whose slice_z is > max_z
    int  idx_vertex_lowest = (vertices[1].z() == min_z) ? 1 : ((vertices[2].z() == min_z) ? 2 : 0);

    for (auto it = min_layer; it != max_layer; ++ it) {
        IntersectionLine il;
        // Ignore horizontal triangles. Any valid horizontal triangle must have a vertical triangle connected, otherwise the part has zero volume.
        if (min_z != max_z && slice_facet(*it, vertices, indices, edge_ids, idx_vertex_lowest, false, il) == FacetSliceType::Slicing) {
            assert(il.edge_type != IntersectionLine::FacetEdgeType::Horizontal);
            size_t slice_id = it - zs.begin();
            boost::lock_guard<std::mutex> l(lines_mutex[slice_id % lines_mutex.size()]);
            lines[slice_id].emplace_back(il);
        }
    }
}

template<typename TransformVertex, typename ThrowOnCancel>
static inline std::vector<IntersectionLines> slice_make_lines(
    const std::vector<stl_vertex>                   &vertices,
    const TransformVertex                           &transform_vertex_fn,
    const std::vector<stl_triangle_vertex_indices>  &indices,
    const std::vector<Vec3i32>                        &face_edge_ids,
    const std::vector<float>                        &zs,
    const ThrowOnCancel                              throw_on_cancel_fn)
{
    std::vector<IntersectionLines>  lines(zs.size(), IntersectionLines());
    std::array<std::mutex, 64>      lines_mutex;
    tbb::parallel_for(
        tbb::blocked_range<int>(0, int(indices.size())),
        [&vertices, &transform_vertex_fn, &indices, &face_edge_ids, &zs, &lines, &lines_mutex, throw_on_cancel_fn](const tbb::blocked_range<int> &range) {
            for (int face_idx = range.begin(); face_idx < range.end(); ++ face_idx) {
                if ((face_idx & 0x0ffff) == 0)
                    throw_on_cancel_fn();
                slice_facet_at_zs(vertices, transform_vertex_fn, indices[face_idx], face_edge_ids[face_idx], zs, lines, lines_mutex);
            }
        }
    );
    return lines;
}

template<typename TransformVertex, typename FaceFilter>
static inline IntersectionLines slice_make_lines(
    const std::vector<stl_vertex>                   &mesh_vertices,
    const TransformVertex                           &transform_vertex_fn,
    const std::vector<stl_triangle_vertex_indices>  &mesh_faces,
    const std::vector<Vec3i32>                        &face_edge_ids,
    const float                                      plane_z, 
    FaceFilter                                       face_filter)
{
    IntersectionLines lines;
    for (int face_idx = 0; face_idx < int(mesh_faces.size()); ++ face_idx)
        if (face_filter(face_idx)) {
            const Vec3i32 &indices = mesh_faces[face_idx];
            stl_vertex vertices[3] { transform_vertex_fn(mesh_vertices[indices(0)]), transform_vertex_fn(mesh_vertices[indices(1)]), transform_vertex_fn(mesh_vertices[indices(2)]) };
            // find facet extents
            const float min_z = fminf(vertices[0].z(), fminf(vertices[1].z(), vertices[2].z()));
            const float max_z = fmaxf(vertices[0].z(), fmaxf(vertices[1].z(), vertices[2].z()));
            assert(min_z <= plane_z && max_z >= plane_z);
            int              idx_vertex_lowest = (vertices[1].z() == min_z) ? 1 : ((vertices[2].z() == min_z) ? 2 : 0);
            IntersectionLine il;
            // Ignore horizontal triangles. Any valid horizontal triangle must have a vertical triangle connected, otherwise the part has zero volume.
            if (min_z != max_z && slice_facet(plane_z, vertices, indices, face_edge_ids[face_idx], idx_vertex_lowest, false, il) == FacetSliceType::Slicing) {
                assert(il.edge_type != IntersectionLine::FacetEdgeType::Horizontal);
                lines.emplace_back(il);
            }
        }
    return lines;
}

// For projecting triangle sets onto slice slabs.
struct SlabLines {
    // Intersection lines of a slice with a triangle set, CCW oriented.
    std::vector<IntersectionLines>  at_slice;
    // Projections of triangle set boundary lines into layer below (for projection from the top)
    // or into layer above (for projection from the bottom).
    // In both cases the intersection lines are CCW oriented.
    std::vector<IntersectionLines>  between_slices;
};

// Orientation of the face normal in regard to a XY plane pointing upwards.
enum class FaceOrientation : char {
    // Z component of the normal is positive.
    Up,
    // Z component of the normal is negative.
    Down,
    // Z component of the normal is zero.
    Vertical,
    // Triangle is degenerate, thus its normal is undefined. We may want to slice the degenerate triangles
    // because of the connectivity information they carry.
    Degenerate
};

template<bool ProjectionFromTop>
void slice_facet_with_slabs(
    // Scaled or unscaled vertices. transform_vertex_fn may scale zs.
    const std::vector<Vec3f>                         &mesh_vertices,
    const std::vector<stl_triangle_vertex_indices>   &mesh_triangles,
    const size_t                                      facet_idx,
    const Vec3i32                                      &facet_neighbors,
    const Vec3i32                                      &facet_edge_ids,
    // Increase edge_ids at the top plane of the slab edges by num_edges to allow chaining
    // from bottom plane of the slab to the top plane of the slab and vice versa.
    const int                                         num_edges,
    const std::vector<float>                         &zs,
    SlabLines                                        &lines,
    std::array<std::mutex, 64>                       &lines_mutex)
{
    const stl_triangle_vertex_indices &indices = mesh_triangles[facet_idx];
    stl_vertex vertices[3] { mesh_vertices[indices(0)], mesh_vertices[indices(1)], mesh_vertices[indices(2)] };

    // find facet extents
    const float min_z = fminf(vertices[0].z(), fminf(vertices[1].z(), vertices[2].z()));
    const float max_z = fmaxf(vertices[0].z(), fmaxf(vertices[1].z(), vertices[2].z()));
    const bool  horizontal = min_z == max_z;

    // find layer extents
    auto min_layer = std::lower_bound(zs.begin(), zs.end(), min_z); // first layer whose slice_z is >= min_z
    auto max_layer = std::upper_bound(min_layer, zs.end(), max_z); // first layer whose slice_z is > max_z
    assert(min_layer == zs.end() ? max_layer == zs.end() : *min_layer >= min_z);
    assert(max_layer == zs.end() || *max_layer > max_z);

    auto emit_slab_edge = [&lines, &lines_mutex](IntersectionLine il, size_t slab_id, bool reverse) {
        if (reverse)
            il.reverse();
        boost::lock_guard<std::mutex> l(lines_mutex[(slab_id + lines_mutex.size() / 2) % lines_mutex.size()]);
        lines.between_slices[slab_id].emplace_back(il);
    };

    if (min_layer == max_layer || horizontal) {
        // Horizontal face or a nearly horizontal face that fits between two layers or below the bottom most or above the top most layer.
        assert(horizontal || zs.empty() || max_z < zs.front() || min_z > zs.back() ||
            (min_layer == max_layer && min_layer != zs.end() && min_layer != zs.begin() && *(min_layer - 1) < min_z && *min_layer > max_z));
        if (horizontal && min_layer != zs.end() && *min_layer == min_z) {
            // Slicing a horizontal triangle with a slicing plane. The triangle has to be upwards facing for ProjectionFromTop
            // and downwards facing for ! ProjectionFromTop.
            assert(min_layer != max_layer);
            // Slicing plane with which the triangle is coplanar.
            size_t slice_id = min_layer - zs.begin();
#if 0
            // Project the coplanar bottom facing triangles to their slicing plane for both top and bottom facing surfaces.
            // This behavior is different from slice_mesh() / slice_mesh_ex(), which do not slice bottom facing faces exactly on slicing plane.
            size_t line_id = slice_id;
#else
            // Project the coplanar bottom facing triangles to the plane above the slicing plane to match the behavior of slice_mesh() / slice_mesh_ex(),
            // where the slicing plane slices the top facing surfaces, but misses the bottom facing surfaces.
            if (size_t line_id = ProjectionFromTop ? slice_id : slice_id + 1; ProjectionFromTop || line_id < lines.at_slice.size())
#endif
                for (int iedge = 0; iedge < 3; ++ iedge)
                    if (facet_neighbors(iedge) == -1) {
                        int i = iedge;
                        int j = next_idx_modulo(i, 3);
                        assert(vertices[i].z() == zs[slice_id]);
                        assert(vertices[j].z() == zs[slice_id]);
                        IntersectionLine il {
                            { to_2d(vertices[i]).cast<coord_t>(), to_2d(vertices[j]).cast<coord_t>() },
                            indices(i), indices(j), -1, -1, 
                            ProjectionFromTop ? IntersectionLine::FacetEdgeType::Bottom : IntersectionLine::FacetEdgeType::Top
                        };
                        // Don't flip the FacetEdgeType::Top edge, it will be flipped when chaining.
                        // if (! ProjectionFromTop) il.reverse();
                        boost::lock_guard<std::mutex> l(lines_mutex[line_id % lines_mutex.size()]);
                        lines.at_slice[line_id].emplace_back(il);
                    }
        } else {
            // Triangle is completely between two slicing planes, the triangle may or may not be horizontal, which 
            // does not matter for the processing of such a triangle.
            size_t slab_id;
            if (ProjectionFromTop) {
                if (max_layer == zs.begin()) {
                    // Not slicing the triangle and it is below the lowest layer.
                    return;
                } else {
                    // Not slicing the triangle and it could be projected into a slab.
                    slab_id = max_layer - zs.begin();
                }
            } else {
                // projection from bottom
                if (min_layer == zs.end()) {
                    // Not slicing the triangle and it is above the highest layer.
                    return;
                } else {
                    // Not slicing the triangle and it could be projected into a slab.
                    slab_id = min_layer - zs.begin();
                }
            }
            if (ProjectionFromTop)
                -- slab_id;
            for (int iedge = 0; iedge < 3; ++ iedge)
                if (facet_neighbors(iedge) == -1) {
                    int i = iedge;
                    int j = next_idx_modulo(i, 3);
                    assert(ProjectionFromTop ? vertices[i].z() >= zs[slab_id] : vertices[i].z() <= zs[slab_id]);
                    assert(ProjectionFromTop ? vertices[j].z() >= zs[slab_id] : vertices[j].z() <= zs[slab_id]);
                    emit_slab_edge(
                        IntersectionLine {
                            { to_2d(vertices[i]).cast<coord_t>(), to_2d(vertices[j]).cast<coord_t>() },
                            indices(i), indices(j), -1, -1, IntersectionLine::FacetEdgeType::Slab
                        },
                        slab_id, ! ProjectionFromTop);
                }
        }
    } else {
        // The triangle is not horizontal and at least a single slicing plane intersects the triangle.
        int idx_vertex_lowest = (vertices[1].z() == min_z) ? 1 : ((vertices[2].z() == min_z) ? 2 : 0);
        IntersectionLine il_prev;
        for (auto it = min_layer; it != max_layer; ++ it) {
            IntersectionLine il;
            auto type = slice_facet(*it, vertices, indices, facet_edge_ids, idx_vertex_lowest, false, il);
            if (type == FacetSliceType::NoSlice) {
                // One and exactly one vertex is touching the slicing plane.
            } else {
                if (il.edge_type == IntersectionLine::FacetEdgeType::Top || il.edge_type == IntersectionLine::FacetEdgeType::Bottom) {
                    // The non-horizontal triangle is being sliced at one of its edges.
                    // If the edge is open (it does not have a neighbor), add it.
                    // If the edge has a neighbor, then add it as TopBottom, and do it just once.
                    assert(il.a_id != -1 && il.b_id != -1);
                    assert(il.edge_a_id == -1 && il.edge_b_id == -1);
                    // Identify edge ID from the edge vertices.
                    int edge_id;
                    if (type == FacetSliceType::Cutting) {
                        // The edge is oriented CCW along the face perimeter.
                        assert(il.edge_type == IntersectionLine::FacetEdgeType::Bottom);
                        edge_id = il.a_id == indices(0) ? 0 : il.a_id == indices(1) ? 1 : 2;
                        assert(il.a_id == indices(edge_id));
                        assert(il.b_id == indices(next_idx_modulo(edge_id, 3)));
                    } else {
                        // The edge is oriented CW along the face perimeter.
                        assert(type == FacetSliceType::Slicing);
                        assert(il.edge_type == IntersectionLine::FacetEdgeType::Top);
                        edge_id = il.b_id == indices(0) ? 0 : il.b_id == indices(1) ? 1 : 2;
                        assert(il.b_id == indices(edge_id));
                        assert(il.a_id == indices(next_idx_modulo(edge_id, 3)));
                    }
                    int neighbor_idx = facet_neighbors(edge_id);
                    if (neighbor_idx == -1) {
                        // Save the open edge for sure.
                        type = FacetSliceType::Slicing;
                    } else {
#ifndef NDEBUG
                        const stl_triangle_vertex_indices &neighbor = mesh_triangles[neighbor_idx];
                        float z = *it;
                        int num_on_plane = (mesh_vertices[neighbor(0)].z() == z) + (mesh_vertices[neighbor(1)].z() == z) + (mesh_vertices[neighbor(2)].z() == z);
                        assert(num_on_plane == 2 || num_on_plane == 3);
#endif // NDEBUG
#if 0
                        if (mesh_vertices[neighbor(0)].z() == z && mesh_vertices[neighbor(1)].z() == z && mesh_vertices[neighbor(2)].z() == z) {
                            // The neighbor triangle is horizontal.
                            // Assign the horizontal projections to slicing planes differently from the usual triangle mesh slicing:
                            // Slicing plane slices top surfaces when projecting from top, it slices bottom surfaces when projecting from bottom.
                            // Is the corner convex or concave?
                            if (il.edge_type == (ProjectionFromTop ? IntersectionLine::FacetEdgeType::Top : IntersectionLine::FacetEdgeType::Bottom)) {
                                // Convex corner. Add this edge to both slabs, the edge is a boundary edge of both the projection patch below and
                                // above this slicing plane.
                                type = FacetSliceType::Slicing;
                                il.edge_type = IntersectionLine::FacetEdgeType::TopBottom;
                            } else {
                                // Concave corner. Ignore this edge, it is internal to the projection patch.
                                type = FacetSliceType::Cutting;
                            }
                        } else 
#else
                            // Project the coplanar bottom facing triangles to the plane above the slicing plane to match the behavior of slice_mesh() / slice_mesh_ex(),
                            // where the slicing plane slices the top facing surfaces, but misses the bottom facing surfaces.
#endif
                        if (il.edge_type == IntersectionLine::FacetEdgeType::Top) {
                            // Indicate that the edge belongs to both the slab below and above the plane.
                            assert(type == FacetSliceType::Slicing);
                            il.edge_type = IntersectionLine::FacetEdgeType::TopBottom;
                        } else {
                            // Don't add this edge, as the neighbor triangle will add the same edge as FacetEdgeType::TopBottom.
                            assert(type == FacetSliceType::Cutting);
                            assert(il.edge_type == IntersectionLine::FacetEdgeType::Bottom);
                        }
                    }
                }
                if (type == FacetSliceType::Slicing) {
                    if (! ProjectionFromTop)
                        il.reverse();
                    size_t line_id = it - zs.begin();
                    boost::lock_guard<std::mutex> l(lines_mutex[line_id % lines_mutex.size()]);
                    lines.at_slice[line_id].emplace_back(il);
                }
            }
            if (! ProjectionFromTop || it != zs.begin()) {
                size_t slab_id = it - zs.begin();
                if (ProjectionFromTop)
                    -- slab_id;
                // Try to project unbound edges.
                for (int iedge = 0; iedge < 3; ++ iedge)
                    if (facet_neighbors(iedge) == -1) {
                        // Unbound edge.
                        int  edge_id         = facet_edge_ids(iedge);
                        bool intersects_this = il.edge_a_id == edge_id || il.edge_b_id == edge_id;
                        bool intersects_prev = il_prev.edge_a_id == edge_id || il_prev.edge_b_id == edge_id;
                        int i = iedge;
                        int j = next_idx_modulo(i, 3);
                        assert((! intersects_this && ! intersects_prev) || vertices[j].z() != vertices[i].z());
                        bool edge_up = vertices[j].z() > vertices[i].z();
                        if (intersects_this && intersects_prev) {
                            // Intersects both, emit the segment between these intersections.
                            Line l(il_prev.edge_a_id == edge_id ? il_prev.a : il_prev.b, 
                                   il.edge_a_id == edge_id ? il.a : il.b);
                            emit_slab_edge(
                                IntersectionLine { l, -1, -1, edge_id, edge_id + num_edges, IntersectionLine::FacetEdgeType::Slab },
                                slab_id, ProjectionFromTop != edge_up);
                        } else if (intersects_this) {
                            // Intersects just the top plane, may touch the bottom plane.
                            assert((vertices[i].z() > *it && vertices[j].z() < *it) || (vertices[i].z() < *it && vertices[j].z() > *it));
                            assert(il.edge_a_id == edge_id || il.edge_b_id == edge_id);
                            emit_slab_edge(
                                IntersectionLine { {
                                        to_2d(edge_up ? vertices[i] : vertices[j]).cast<coord_t>(),
                                        il.edge_a_id == edge_id ? il.a : il.b
                                    },
                                    edge_up ? indices(i) : indices(j), -1, -1, edge_id + num_edges, IntersectionLine::FacetEdgeType::Slab
                                },
                                slab_id, ProjectionFromTop != edge_up);
                        } else if (intersects_prev) {
                            // Intersects just the bottom plane, may touch the top vertex.
                            assert(*it <= max_z);
#ifndef NDEBUG
                            {
                                auto it_prev = it;
                                -- it_prev;
                                assert((vertices[i].z() > *it_prev && vertices[j].z() < *it_prev) || (vertices[i].z() < *it_prev && vertices[j].z() > *it_prev));
                            }
#endif // NDEBUG
                            emit_slab_edge(
                                IntersectionLine { {
                                        il_prev.edge_a_id == edge_id ? il_prev.a : il_prev.b,
                                        to_2d(edge_up ? vertices[j] : vertices[i]).cast<coord_t>()
                                    },
                                    -1, edge_up ? indices(j) : indices(i), edge_id, -1, IntersectionLine::FacetEdgeType::Slab
                                },
                                slab_id, ProjectionFromTop != edge_up);
                        } else if (float zi = vertices[i].z(), zj = vertices[j].z(); zi < *it || zj < *it) {
                            // The edge does not intersect the current plane and it does not intersect the previous plane either.
                            // Both points have to be inside the slab.
                            assert(zi <= *it && zj <= *it);
#ifndef NDEBUG
                            if (type == FacetSliceType::Slicing || type == FacetSliceType::Cutting) {
                                // Such edge should already be processed in the code above, it shall be skipped here.
                                assert(indices(i) != il.b_id || indices(j) != il.a_id);
                                assert(indices(i) != il.a_id || indices(j) != il.b_id);
                            }
#endif // NDEBUG
                            // Is it inside the slab?
                            bool inside_slab = true;
                            if (it != min_layer) {
                                auto it_prev = it;
                                -- it_prev;
                                assert(*it_prev >= *min_layer && *it_prev < *it);
                                // One point may touch the plane below, the other must not.
                                inside_slab = zi > *it_prev || zj > *it_prev;
                                // Both points have to be inside the slab.
                                assert(! inside_slab || (zi >= *it_prev && zj >= *it_prev));
                            }
                            if (inside_slab) {
                                assert(ProjectionFromTop ? vertices[i].z() >= zs[slab_id] : vertices[i].z() <= zs[slab_id]);
                                assert(ProjectionFromTop ? vertices[j].z() >= zs[slab_id] : vertices[j].z() <= zs[slab_id]);
                                emit_slab_edge(
                                    IntersectionLine {
                                        { to_2d(vertices[i]).cast<coord_t>(), to_2d(vertices[j]).cast<coord_t>() },
                                        indices(i), indices(j), -1, -1, IntersectionLine::FacetEdgeType::Slab
                                    },
                                    slab_id, ! ProjectionFromTop);
                            }
                        }
                    }
            }
            il_prev = il;
        }
        if (ProjectionFromTop || max_layer != zs.end()) {
            // Try to project unbound edges above the last slicing plane to the last slab.
            // Last layer slicing this triangle.
            auto   it      = max_layer - 1;
            size_t slab_id = max_layer - zs.begin();
            if (ProjectionFromTop)
                -- slab_id;
            for (int iedge = 0; iedge < 3; ++ iedge)
                if (facet_neighbors(iedge) == -1) {
                    // Unbound edge.
                    int edge_id = facet_edge_ids(iedge);
                    int i = iedge;
                    int j = next_idx_modulo(i, 3);
                    if (il_prev.edge_a_id == edge_id || il_prev.edge_b_id == edge_id) {
                        // Intersects just the bottom plane, may touch the top vertex.
                        assert((vertices[i].z() > *it && vertices[j].z() < *it) || (vertices[i].z() < *it && vertices[j].z() > *it));
                        bool edge_up = vertices[j].z() > vertices[i].z();
                        emit_slab_edge(
                            IntersectionLine{ {
                                    il_prev.edge_a_id == edge_id ? il_prev.a : il_prev.b,
                                    to_2d(edge_up ? vertices[j] : vertices[i]).cast<coord_t>()
                                },
                                -1, edge_up ? indices(j) : indices(i), edge_id, -1, IntersectionLine::FacetEdgeType::Slab
                            },
                            slab_id, ProjectionFromTop != edge_up);
                    } else if (float zi = vertices[i].z(), zj = vertices[j].z(); zi > *it || zj > *it) {
                        // The edge does not intersect the current plane and it does not intersect the previous plane either.
                        // Both points have to be inside the slab.
                        assert(zi >= *it && zj >= *it);
                        assert(max_layer == zs.end() || (zi < *max_layer && zj < *max_layer));
                        emit_slab_edge(
                            IntersectionLine{
                                { to_2d(vertices[i]).cast<coord_t>(), to_2d(vertices[j]).cast<coord_t>() },
                                indices(i), indices(j), -1, -1, IntersectionLine::FacetEdgeType::Slab
                            },
                            slab_id, ! ProjectionFromTop);
                    }
                }
        }
    }
}

// used by slice_mesh_slabs() to produce on-slice lines and between-slices lines.
// Returning top / bottom SlabLines.
template<typename ThrowOnCancel>
inline std::pair<SlabLines, SlabLines> slice_slabs_make_lines(
    const std::vector<stl_vertex>                   &vertices,
    const std::vector<stl_triangle_vertex_indices>  &indices,
    const std::vector<Vec3i32>                        &face_neighbors,
    const std::vector<Vec3i32>                        &face_edge_ids,
    // Total number of edges. All face_edge_ids are lower than num_edges.
    // num_edges will be used to distinguish between intersections with the top and bottom plane.
    const int                                        num_edges,
    const std::vector<FaceOrientation>              &face_orientation,
    const std::vector<float>                        &zs,
    bool                                             top,
    bool                                             bottom,
    const ThrowOnCancel                              throw_on_cancel_fn)
{
    std::pair<SlabLines, SlabLines> out;
    SlabLines   &lines_top      = out.first;
    SlabLines   &lines_bottom   = out.second;
    std::array<std::mutex, 64> lines_mutex_top;
    std::array<std::mutex, 64> lines_mutex_bottom;

    if (top) {
        lines_top.at_slice.assign(zs.size(), IntersectionLines());
        lines_top.between_slices.assign(zs.size(), IntersectionLines());
    }
    if (bottom) {
        lines_bottom.at_slice.assign(zs.size(), IntersectionLines());
        lines_bottom.between_slices.assign(zs.size(), IntersectionLines());        
    }

    tbb::parallel_for(
        tbb::blocked_range<int>(0, int(indices.size())),
        [&vertices, &indices, &face_neighbors, &face_edge_ids, num_edges, &face_orientation, &zs, top, bottom, &lines_top, &lines_bottom, &lines_mutex_top, &lines_mutex_bottom, throw_on_cancel_fn]
        (const tbb::blocked_range<int> &range) {
            for (int face_idx = range.begin(); face_idx < range.end(); ++ face_idx) {
                if ((face_idx & 0x0ffff) == 0)
                    throw_on_cancel_fn();
                FaceOrientation fo       = face_orientation[face_idx];
                Vec3i32           edge_ids = face_edge_ids[face_idx];
                if (top && (fo == FaceOrientation::Up || fo == FaceOrientation::Degenerate)) {
                    Vec3i32 neighbors = face_neighbors[face_idx];
                    // Reset neighborship of this triangle in case the other triangle is oriented backwards from this one.
                    for (int i = 0; i < 3; ++ i)
                        if (neighbors(i) != -1) {
                            FaceOrientation fo2 = face_orientation[neighbors(i)];
                            if (fo2 != FaceOrientation::Up && fo2 != FaceOrientation::Degenerate)
                                neighbors(i) = -1;
                        }
                    slice_facet_with_slabs<true>(vertices, indices, face_idx, neighbors, edge_ids, num_edges, zs, lines_top, lines_mutex_top);
                }
                if (bottom && (fo == FaceOrientation::Down || fo == FaceOrientation::Degenerate)) {
                    Vec3i32 neighbors = face_neighbors[face_idx];
                    // Reset neighborship of this triangle in case the other triangle is oriented backwards from this one.
                    for (int i = 0; i < 3; ++ i)
                        if (neighbors(i) != -1) {
                            FaceOrientation fo2 = face_orientation[neighbors(i)];
                            if (fo2 != FaceOrientation::Down && fo2 != FaceOrientation::Degenerate)
                                neighbors(i) = -1;
                        }
                    slice_facet_with_slabs<false>(vertices, indices, face_idx, neighbors, edge_ids, num_edges, zs, lines_bottom, lines_mutex_bottom);
                }
            }
        }
    );
    return out;
}

#if 0
//FIXME Should this go away? For valid meshes the function slice_facet() returns Slicing
// and sets edges of vertical triangles to produce only a single edge per pair of neighbor faces.
// So the following code makes only sense now to handle degenerate meshes with more than two faces
// sharing a single edge.
static inline void remove_tangent_edges(std::vector<IntersectionLine> &lines)
{
    std::vector<IntersectionLine*> by_vertex_pair;
    by_vertex_pair.reserve(lines.size());
    for (IntersectionLine& line : lines)
        if (line.edge_type != IntersectionLine::FacetEdgeType::General && line.a_id != -1)
            // This is a face edge. Check whether there is its neighbor stored in lines.
            by_vertex_pair.emplace_back(&line);
    auto edges_lower_sorted = [](const IntersectionLine *l1, const IntersectionLine *l2) {
        // Sort vertices of l1, l2 lexicographically
        int l1a = l1->a_id;
        int l1b = l1->b_id;
        int l2a = l2->a_id;
        int l2b = l2->b_id;
        if (l1a > l1b)
            std::swap(l1a, l1b);
        if (l2a > l2b)
            std::swap(l2a, l2b);
        // Lexicographical "lower" operator on lexicographically sorted vertices should bring equal edges together when sored.
        return l1a < l2a || (l1a == l2a && l1b < l2b);
    };
    std::sort(by_vertex_pair.begin(), by_vertex_pair.end(), edges_lower_sorted);
    for (auto line = by_vertex_pair.begin(); line != by_vertex_pair.end(); ++ line) {
        IntersectionLine &l1 = **line;
        if (! l1.skip()) {
            // Iterate as long as line and line2 edges share the same end points.
            for (auto line2 = line + 1; line2 != by_vertex_pair.end() && ! edges_lower_sorted(*line, *line2); ++ line2) {
                // Lines must share the end points.
                assert(! edges_lower_sorted(*line, *line2));
                assert(! edges_lower_sorted(*line2, *line));
                IntersectionLine &l2 = **line2;
                if (l2.skip())
                    continue;
                if (l1.a_id == l2.a_id) {
                    assert(l1.b_id == l2.b_id);
                    l2.set_skip();
                    // If they are both oriented upwards or downwards (like a 'V'),
                    // then we can remove both edges from this layer since it won't 
                    // affect the sliced shape.
                    // If one of them is oriented upwards and the other is oriented
                    // downwards, let's only keep one of them (it doesn't matter which
                    // one since all 'top' lines were reversed at slicing).
                    if (l1.edge_type == l2.edge_type) {
                        l1.set_skip();
                        break;
                    }
                } else {
                    assert(l1.a_id == l2.b_id && l1.b_id == l2.a_id);
                    // If this edge joins two horizontal facets, remove both of them.
                    if (l1.edge_type == IntersectionLine::FacetEdgeType::Horizontal && l2.edge_type == IntersectionLine::FacetEdgeType::Horizontal) {
                        l1.set_skip();
                        l2.set_skip();
                        break;
                    }
                }
            }
        }
    }
}
#endif

struct OpenPolyline {
    OpenPolyline() = default;
    OpenPolyline(const IntersectionReference &start, const IntersectionReference &end, Points &&points) : 
        start(start), end(end), points(std::move(points)), consumed(false) { this->length = Slic3r::length(this->points); }
    void reverse() {
        std::swap(start, end);
        std::reverse(points.begin(), points.end());
    }
    IntersectionReference   start;
    IntersectionReference   end;
    Points                  points;
    double                  length;
    bool                    consumed;
};

// called by make_loops() to connect sliced triangles into closed loops and open polylines by the triangle connectivity.
// Only connects segments crossing triangles of the same orientation.
static void chain_lines_by_triangle_connectivity(IntersectionLines &lines, Polygons &loops, std::vector<OpenPolyline> &open_polylines)
{
    // Build a map of lines by edge_a_id and a_id.
    std::vector<IntersectionLine*> by_edge_a_id;
    std::vector<IntersectionLine*> by_a_id;
    by_edge_a_id.reserve(lines.size());
    by_a_id.reserve(lines.size());
    for (IntersectionLine &line : lines) {
        if (! line.skip()) {
            if (line.edge_a_id != -1)
                by_edge_a_id.emplace_back(&line);
            if (line.a_id != -1)
                by_a_id.emplace_back(&line);
        }
    }
    auto by_edge_lower = [](const IntersectionLine* il1, const IntersectionLine *il2) { return il1->edge_a_id < il2->edge_a_id; };
    auto by_vertex_lower = [](const IntersectionLine* il1, const IntersectionLine *il2) { return il1->a_id < il2->a_id; };
    std::sort(by_edge_a_id.begin(), by_edge_a_id.end(), by_edge_lower);
    std::sort(by_a_id.begin(), by_a_id.end(), by_vertex_lower);
    // Chain the segments with a greedy algorithm, collect the loops and unclosed polylines.
    IntersectionLines::iterator it_line_seed = lines.begin();
    for (;;) {
        // take first spare line and start a new loop
        IntersectionLine *first_line = nullptr;
        for (; it_line_seed != lines.end(); ++ it_line_seed)
            if (it_line_seed->is_seed_candidate()) {
            //if (! it_line_seed->skip()) {
                first_line = &(*it_line_seed ++);
                break;
            }
        if (first_line == nullptr)
            break;
        first_line->set_skip();
        Points loop_pts;
        loop_pts.emplace_back(first_line->a);
        IntersectionLine *last_line = first_line;
        
        /*
        printf("first_line edge_a_id = %d, edge_b_id = %d, a_id = %d, b_id = %d, a = %d,%d, b = %d,%d\n", 
            first_line->edge_a_id, first_line->edge_b_id, first_line->a_id, first_line->b_id,
            first_line->a.x, first_line->a.y, first_line->b.x, first_line->b.y);
        */
        
        IntersectionLine key;
        for (;;) {
            // find a line starting where last one finishes
            IntersectionLine* next_line = nullptr;
            if (last_line->edge_b_id != -1) {
                key.edge_a_id = last_line->edge_b_id;
                auto it_begin = std::lower_bound(by_edge_a_id.begin(), by_edge_a_id.end(), &key, by_edge_lower);
                if (it_begin != by_edge_a_id.end()) {
                    auto it_end = std::upper_bound(it_begin, by_edge_a_id.end(), &key, by_edge_lower);
                    for (auto it_line = it_begin; it_line != it_end; ++ it_line)
                        if (! (*it_line)->skip()) {
                            next_line = *it_line;
                            break;
                        }
                }
            }
            if (next_line == nullptr && last_line->b_id != -1) {
                key.a_id = last_line->b_id;
                auto it_begin = std::lower_bound(by_a_id.begin(), by_a_id.end(), &key, by_vertex_lower);
                if (it_begin != by_a_id.end()) {
                    auto it_end = std::upper_bound(it_begin, by_a_id.end(), &key, by_vertex_lower);
                    for (auto it_line = it_begin; it_line != it_end; ++ it_line)
                        if (! (*it_line)->skip()) {
                            next_line = *it_line;
                            break;
                        }
                }
            }
            if (next_line == nullptr) {
                // Check whether we closed this loop.
                if ((first_line->edge_a_id != -1 && first_line->edge_a_id == last_line->edge_b_id) || 
                    (first_line->a_id      != -1 && first_line->a_id      == last_line->b_id)) {
                    // The current loop is complete. Add it to the output.
                    assert(first_line->a == last_line->b);
                    loops.emplace_back(std::move(loop_pts));
                    #ifdef SLIC3R_TRIANGLEMESH_DEBUG
                    printf("  Discovered %s polygon of %d points\n", (p.is_counter_clockwise() ? "ccw" : "cw"), (int)p.points.size());
                    #endif
                } else {
                    // This is an open polyline. Add it to the list of open polylines. These open polylines will processed later.
                    loop_pts.emplace_back(last_line->b);
                    open_polylines.emplace_back(OpenPolyline(
                        IntersectionReference(first_line->a_id, first_line->edge_a_id), 
                        IntersectionReference(last_line->b_id, last_line->edge_b_id), std::move(loop_pts)));
                }
                break;
            }
            /*
            printf("next_line edge_a_id = %d, edge_b_id = %d, a_id = %d, b_id = %d, a = %d,%d, b = %d,%d\n", 
                next_line->edge_a_id, next_line->edge_b_id, next_line->a_id, next_line->b_id,
                next_line->a.x, next_line->a.y, next_line->b.x, next_line->b.y);
            */
            assert(last_line->b == next_line->a);
            loop_pts.emplace_back(next_line->a);
            last_line = next_line;
            next_line->set_skip();
        }
    }
}

std::vector<OpenPolyline*> open_polylines_sorted(std::vector<OpenPolyline> &open_polylines, bool update_lengths)
{
    std::vector<OpenPolyline*> out;
    out.reserve(open_polylines.size());
    for (OpenPolyline &opl : open_polylines)
        if (! opl.consumed) {
            if (update_lengths)
                opl.length = Slic3r::length(opl.points);
            out.emplace_back(&opl);
        }
    std::sort(out.begin(), out.end(), [](const OpenPolyline *lhs, const OpenPolyline *rhs){ return lhs->length > rhs->length; });
    return out;
}

// called by make_loops() to connect remaining open polylines across shared triangle edges and vertices.
// Depending on "try_connect_reversed", it may or may not connect segments crossing triangles of opposite orientation.
static void chain_open_polylines_exact(std::vector<OpenPolyline> &open_polylines, Polygons &loops, bool try_connect_reversed)
{
    // Store the end points of open_polylines into vectors sorted
    struct OpenPolylineEnd {
        OpenPolylineEnd(OpenPolyline *polyline, bool start) : polyline(polyline), start(start) {}
        OpenPolyline    *polyline;
        // Is it the start or end point?
        bool             start;
        const IntersectionReference& ipref() const { return start ? polyline->start : polyline->end; }
        // Return a unique ID for the intersection point.
        // Return a positive id for a point, or a negative id for an edge.
        int id() const { const IntersectionReference &r = ipref(); return (r.point_id >= 0) ? r.point_id : - r.edge_id; }
        bool operator==(const OpenPolylineEnd &rhs) const { return this->polyline == rhs.polyline && this->start == rhs.start; }
    };
    auto by_id_lower = [](const OpenPolylineEnd &ope1, const OpenPolylineEnd &ope2) { return ope1.id() < ope2.id(); };
    std::vector<OpenPolylineEnd> by_id;
    by_id.reserve(2 * open_polylines.size());
    for (OpenPolyline &opl : open_polylines) {
        if (opl.start.point_id != -1 || opl.start.edge_id != -1)
            by_id.emplace_back(OpenPolylineEnd(&opl, true));
        if (try_connect_reversed && (opl.end.point_id != -1 || opl.end.edge_id != -1))
            by_id.emplace_back(OpenPolylineEnd(&opl, false));
    }
    std::sort(by_id.begin(), by_id.end(), by_id_lower);
    // Find an iterator to by_id_lower for the particular end of OpenPolyline (by comparing the OpenPolyline pointer and the start attribute).
    auto find_polyline_end = [&by_id, by_id_lower](const OpenPolylineEnd &end) -> std::vector<OpenPolylineEnd>::iterator {
        for (auto it = std::lower_bound(by_id.begin(), by_id.end(), end, by_id_lower);
                  it != by_id.end() && it->id() == end.id(); ++ it)
            if (*it == end)
                return it;
        return by_id.end();
    };
    // Try to connect the loops.
    std::vector<OpenPolyline*> sorted_by_length = open_polylines_sorted(open_polylines, false);
    for (OpenPolyline *opl : sorted_by_length) {
        if (opl->consumed)
            continue;
        opl->consumed = true;
        OpenPolylineEnd end(opl, false);
        for (;;) {
            // find a line starting where last one finishes
            auto it_next_start = std::lower_bound(by_id.begin(), by_id.end(), end, by_id_lower);
            for (; it_next_start != by_id.end() && it_next_start->id() == end.id(); ++ it_next_start)
                if (! it_next_start->polyline->consumed)
                    goto found;
            // The current loop could not be closed. Unmark the segment.
            opl->consumed = false;
            break;
        found:
            // Attach this polyline to the end of the initial polyline.
            if (it_next_start->start) {
                auto it = it_next_start->polyline->points.begin();
                std::copy(++ it, it_next_start->polyline->points.end(), back_inserter(opl->points));
            } else {
                auto it = it_next_start->polyline->points.rbegin();
                std::copy(++ it, it_next_start->polyline->points.rend(), back_inserter(opl->points));
            }
            opl->length += it_next_start->polyline->length;
            // Mark the next polyline as consumed.
            it_next_start->polyline->points.clear();
            it_next_start->polyline->length = 0.;
            it_next_start->polyline->consumed = true;
            if (try_connect_reversed) {
                // Running in a mode, where the polylines may be connected by mixing their orientations.
                // Update the end point lookup structure after the end point of the current polyline was extended.
                auto it_end      = find_polyline_end(end);
                auto it_next_end = find_polyline_end(OpenPolylineEnd(it_next_start->polyline, !it_next_start->start));
                // Swap the end points of the current and next polyline, but keep the polyline ptr and the start flag.
                std::swap(opl->end, it_next_end->start ? it_next_end->polyline->start : it_next_end->polyline->end);
                // Swap the positions of OpenPolylineEnd structures in the sorted array to match their respective end point positions.
                std::swap(*it_end, *it_next_end);
            }
            // Check whether we closed this loop.
            if ((opl->start.edge_id  != -1 && opl->start.edge_id  == opl->end.edge_id) ||
                (opl->start.point_id != -1 && opl->start.point_id == opl->end.point_id)) {
                // The current loop is complete. Add it to the output.
                //assert(opl->points.front().point_id == opl->points.back().point_id);
                //assert(opl->points.front().edge_id  == opl->points.back().edge_id);
                // Remove the duplicate last point.
                opl->points.pop_back();
                if (opl->points.size() >= 3) {
                    if (try_connect_reversed && area(opl->points) < 0)
                        // The closed polygon is patched from pieces with messed up orientation, therefore
                        // the orientation of the patched up polygon is not known.
                        // Orient the patched up polygons CCW. This heuristic may close some holes and cavities.
                        std::reverse(opl->points.begin(), opl->points.end());
                    loops.emplace_back(std::move(opl->points));
                }
                opl->points.clear();
                break;
            }
            // Continue with the current loop.
        }
    }
}

// called by make_loops() to connect remaining open polylines across shared triangle edges and vertices, 
// possibly closing small gaps.
// Depending on "try_connect_reversed", it may or may not connect segments crossing triangles of opposite orientation.
static void chain_open_polylines_close_gaps(std::vector<OpenPolyline> &open_polylines, Polygons &loops, double max_gap, bool try_connect_reversed)
{
    const coord_t max_gap_scaled = (coord_t)scale_(max_gap);

    // Sort the open polylines by their length, so the new loops will be seeded from longer chains.
    // Update the polyline lengths, return only not yet consumed polylines.
    std::vector<OpenPolyline*> sorted_by_length = open_polylines_sorted(open_polylines, true);

    // Store the end points of open_polylines into ClosestPointInRadiusLookup<OpenPolylineEnd>.
    struct OpenPolylineEnd {
        OpenPolylineEnd(OpenPolyline *polyline, bool start) : polyline(polyline), start(start) {}
        OpenPolyline    *polyline;
        // Is it the start or end point?
        bool             start;
        const Point&     point() const { return start ? polyline->points.front() : polyline->points.back(); }
        bool operator==(const OpenPolylineEnd &rhs) const { return this->polyline == rhs.polyline && this->start == rhs.start; }
    };
    struct OpenPolylineEndAccessor {
        const Point* operator()(const OpenPolylineEnd &pt) const { return pt.polyline->consumed ? nullptr : &pt.point(); }
    };
    typedef ClosestPointInRadiusLookup<OpenPolylineEnd, OpenPolylineEndAccessor> ClosestPointLookupType;
    ClosestPointLookupType closest_end_point_lookup(max_gap_scaled);
    for (OpenPolyline *opl : sorted_by_length) {
        closest_end_point_lookup.insert(OpenPolylineEnd(opl, true));
        if (try_connect_reversed)
            closest_end_point_lookup.insert(OpenPolylineEnd(opl, false));
    }
    // Try to connect the loops.
    for (OpenPolyline *opl : sorted_by_length) {
        if (opl->consumed)
            continue;
        OpenPolylineEnd end(opl, false);
        if (try_connect_reversed)
            // The end point of this polyline will be modified, thus the following entry will become invalid. Remove it.
            closest_end_point_lookup.erase(end);
        opl->consumed = true;
        size_t n_segments_joined = 1;
        for (;;) {
            // Find a line starting where last one finishes, only return non-consumed open polylines (OpenPolylineEndAccessor returns null for consumed).
            std::pair<const OpenPolylineEnd*, double> next_start_and_dist = closest_end_point_lookup.find(end.point());
            const OpenPolylineEnd *next_start = next_start_and_dist.first;
            // Check whether we closed this loop.
            double current_loop_closing_distance2 = (opl->points.back() - opl->points.front()).cast<double>().squaredNorm();
            bool   loop_closed = current_loop_closing_distance2 < coordf_t(max_gap_scaled) * coordf_t(max_gap_scaled);
            if (next_start != nullptr && loop_closed && current_loop_closing_distance2 < next_start_and_dist.second) {
                // Heuristics to decide, whether to close the loop, or connect another polyline.
                // One should avoid closing loops shorter than max_gap_scaled.
                loop_closed = sqrt(current_loop_closing_distance2) < 0.3 * length(opl->points);
            }
            if (loop_closed) {
                // Remove the start point of the current polyline from the lookup.
                // Mark the current segment as not consumed, otherwise the closest_end_point_lookup.erase() would fail.
                opl->consumed = false;
                closest_end_point_lookup.erase(OpenPolylineEnd(opl, true));
                if (current_loop_closing_distance2 == 0.) {
                    // Remove the duplicate last point.
                    opl->points.pop_back();
                } else {
                    // The end points are different, keep both of them.
                }
                if (opl->points.size() >= 3) {
                    if (try_connect_reversed && n_segments_joined > 1 && area(opl->points) < 0)
                        // The closed polygon is patched from pieces with messed up orientation, therefore
                        // the orientation of the patched up polygon is not known.
                        // Orient the patched up polygons CCW. This heuristic may close some holes and cavities.
                        std::reverse(opl->points.begin(), opl->points.end());
                    loops.emplace_back(std::move(opl->points));
                }
                opl->points.clear();
                opl->consumed = true;
                break;
            }
            if (next_start == nullptr) {
                // The current loop could not be closed. Unmark the segment.
                opl->consumed = false;
                if (try_connect_reversed)
                    // Re-insert the end point.
                    closest_end_point_lookup.insert(OpenPolylineEnd(opl, false));
                break;
            }
            // Attach this polyline to the end of the initial polyline.
            if (next_start->start) {
                auto it = next_start->polyline->points.begin();
                if (*it == opl->points.back())
                    ++ it;
                std::copy(it, next_start->polyline->points.end(), back_inserter(opl->points));
            } else {
                auto it = next_start->polyline->points.rbegin();
                if (*it == opl->points.back())
                    ++ it;
                std::copy(it, next_start->polyline->points.rend(), back_inserter(opl->points));
            }
            ++ n_segments_joined;
            // Remove the end points of the consumed polyline segment from the lookup.
            OpenPolyline *opl2 = next_start->polyline;
            closest_end_point_lookup.erase(OpenPolylineEnd(opl2, true));
            if (try_connect_reversed)
                closest_end_point_lookup.erase(OpenPolylineEnd(opl2, false));
            opl2->points.clear();
            opl2->consumed = true;
            // Continue with the current loop.
        }
    }
}

static Polygons make_loops(
    // Lines will have their flags modified.
    IntersectionLines   &lines)
{
    Polygons loops;
#if 0
//FIXME slice_facet() may create zero length edges due to rounding of doubles into coord_t.
//#ifdef _DEBUG
    for (const Line &l : lines)
        assert(l.a != l.b);
#endif /* _DEBUG */

    // There should be no tangent edges, as the horizontal triangles are ignored and if two triangles touch at a cutting plane,
    // only the bottom triangle is considered to be cutting the plane.
//    remove_tangent_edges(lines);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        BoundingBox bbox_svg;
        {
            static int iRun = 0;
            for (const Line &line : lines) {
                bbox_svg.merge(line.a);
                bbox_svg.merge(line.b);
            }
            SVG svg(debug_out_path("TriangleMeshSlicer_make_loops-raw_lines-%d.svg", iRun ++).c_str(), bbox_svg);
            for (const Line &line : lines)
                svg.draw(line);
            svg.Close();
        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    std::vector<OpenPolyline> open_polylines;
    chain_lines_by_triangle_connectivity(lines, loops, open_polylines);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("TriangleMeshSlicer_make_loops-polylines-%d.svg", iRun ++).c_str(), bbox_svg);
            svg.draw(union_ex(loops));
            for (const OpenPolyline &pl : open_polylines)
                svg.draw(Polyline(pl.points), "red");
            svg.Close();
        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Now process the open polylines.
    // Do it in two rounds, first try to connect in the same direction only,
    // then try to connect the open polylines in reversed order as well.
    chain_open_polylines_exact(open_polylines, loops, false);
    chain_open_polylines_exact(open_polylines, loops, true);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        SVG svg(debug_out_path("TriangleMeshSlicer_make_loops-polylines2-%d.svg", iRun++).c_str(), bbox_svg);
        svg.draw(union_ex(loops));
        for (const OpenPolyline &pl : open_polylines) {
            if (pl.points.empty())
                continue;
            svg.draw(Polyline(pl.points), "red");
            svg.draw(pl.points.front(), "blue");
            svg.draw(pl.points.back(), "blue");
        }
        svg.Close();
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Try to close gaps.
    // Do it in two rounds, first try to connect in the same direction only,
    // then try to connect the open polylines in reversed order as well.
#if 0
    for (double max_gap : { EPSILON, 0.001, 0.1, 1., 2. }) {
        chain_open_polylines_close_gaps(open_polylines, *loops, max_gap, false);
        chain_open_polylines_close_gaps(open_polylines, *loops, max_gap, true);
    }
#else
    const double max_gap = 2.; //mm
    chain_open_polylines_close_gaps(open_polylines, loops, max_gap, false);
    chain_open_polylines_close_gaps(open_polylines, loops, max_gap, true);
#endif

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    {
        static int iRun = 0;
        SVG svg(debug_out_path("TriangleMeshSlicer_make_loops-polylines-final-%d.svg", iRun++).c_str(), bbox_svg);
        svg.draw(union_ex(loops));
        for (const OpenPolyline &pl : open_polylines) {
            if (pl.points.empty())
                continue;
            svg.draw(Polyline(pl.points), "red");
            svg.draw(pl.points.front(), "blue");
            svg.draw(pl.points.back(), "blue");
        }
        svg.Close();
    }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    return loops;
}

template<typename ThrowOnCancel>
static std::vector<Polygons> make_loops(
    // Lines will have their flags modified.
    std::vector<IntersectionLines> &lines, 
    const MeshSlicingParams        &params, 
    ThrowOnCancel                   throw_on_cancel)
{
    std::vector<Polygons> layers;
    layers.resize(lines.size());
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, lines.size()),
        [&lines, &layers, &params, throw_on_cancel](const tbb::blocked_range<size_t> &range) {
            for (size_t line_idx = range.begin(); line_idx < range.end(); ++ line_idx) {
                if ((line_idx & 0x0ffff) == 0)
                    throw_on_cancel();

                Polygons &polygons = layers[line_idx];
                polygons = make_loops(lines[line_idx]);

                auto this_mode = line_idx < params.slicing_mode_normal_below_layer ? params.mode_below : params.mode;
                if (! polygons.empty()) {
                    if (this_mode == MeshSlicingParams::SlicingMode::Positive) {
                        // Reorient all loops to be CCW.
                        for (Polygon& p : polygons)
                            p.make_counter_clockwise();
                    }
                    else if (this_mode == MeshSlicingParams::SlicingMode::PositiveLargestContour) {
                        // Keep just the largest polygon, make it CCW.
                        double   max_area = 0.;
                        Polygon* max_area_polygon = nullptr;
                        for (Polygon& p : polygons) {
                            double a = p.area();
                            if (std::abs(a) > std::abs(max_area)) {
                                max_area = a;
                                max_area_polygon = &p;
                            }
                        }
                        assert(max_area_polygon != nullptr);
                        if (max_area < 0.)
                            max_area_polygon->reverse();
                        Polygon p(std::move(*max_area_polygon));
                        polygons.clear();
                        polygons.emplace_back(std::move(p));
                    }
                }
            }
        }
    );

    return layers;
}

// used by slice_mesh_slabs() to produce loops from on-slice lines and between-slices lines.
template<bool ProjectionFromTop, typename ThrowOnCancel>
static std::vector<Polygons> make_slab_loops(
    // Lines will have their flags modified.
    SlabLines                      &lines, 
    // To differentiate edge IDs of the top plane from the edge IDs of the bottom plane for chaining.
    int                             num_edges,
    ThrowOnCancel                   throw_on_cancel)
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    static int iRun = 0;
    ++ iRun;
#endif // SLIC3R_DEBUG_SLICE_PROCESSING

    assert(! lines.at_slice.empty() && lines.at_slice.size() == lines.between_slices.size());
    std::vector<Polygons> layers;
    layers.resize(lines.at_slice.size());
    tbb::parallel_for(
        tbb::blocked_range<int>(0, int(lines.at_slice.size())),
        [&lines, num_edges, &layers, throw_on_cancel](const tbb::blocked_range<int> &range) {
            for (int line_idx = range.begin(); line_idx < range.end(); ++ line_idx) {
                if ((line_idx & 0x0ffff) == 0)
                    throw_on_cancel();
                IntersectionLines in;
                size_t nlines          = lines.between_slices[line_idx].size();
                int    slice_below     = ProjectionFromTop ? line_idx : line_idx - 1;
                int    slice_above     = ProjectionFromTop ? line_idx + 1 : line_idx;
                bool   has_slice_below = ProjectionFromTop || line_idx > 0;
                bool   has_slice_above = ! ProjectionFromTop || line_idx + 1 < int(lines.at_slice.size());
                if (has_slice_below)
                    nlines += lines.at_slice[slice_below].size();
                if (has_slice_above)
                    nlines += lines.at_slice[slice_above].size();
                if (nlines) {
                    in.reserve(nlines);
                    if (has_slice_below) {
                        for (const IntersectionLine &l : lines.at_slice[slice_below])
                            if (l.edge_type != IntersectionLine::FacetEdgeType::Top) {
                                in.emplace_back(l);
#ifndef NDEBUG
                                in.back().source = IntersectionLine::Source::BottomPlane;
#endif // NDEBUG
                            }
                    }
                    {
                        // Edges in between slice_below and slice_above.
#ifndef NDEBUG
                        size_t old_size = in.size();
#endif // NDEBUG
                        // Edge IDs of end points on in-between lines that touch the layer above are already increased with num_edges.
                        append(in, lines.between_slices[line_idx]);
#ifndef NDEBUG
                        for (auto it = in.begin() + old_size; it != in.end(); ++ it) {
                            assert(it->edge_type == IntersectionLine::FacetEdgeType::Slab);
                            it->source = IntersectionLine::Source::Slab;
                        }
#endif // NDEBUG
                    }
                    if (has_slice_above) {
                        for (const IntersectionLine &lsrc : lines.at_slice[slice_above])
                            if (lsrc.edge_type != IntersectionLine::FacetEdgeType::Bottom) {
                                in.emplace_back(lsrc);
                                auto &l = in.back();
                                l.reverse();
                                // Differentiate edge IDs of the top plane from the edge IDs of the bottom plane for chaining.
                                if (l.edge_a_id >= 0)
                                    l.edge_a_id += num_edges;
                                if (l.edge_b_id >= 0)
                                    l.edge_b_id += num_edges;
#ifndef NDEBUG
                                l.source = IntersectionLine::Source::TopPlane;
#endif // NDEBUG
                            }
                    }
                    if (! in.empty()) {
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        BoundingBox bbox_svg;
                        coordf_t stroke_width = scale_(0.02);
                        {
                            for (const IntersectionLine &line : in) {
                                bbox_svg.merge(line.a);
                                bbox_svg.merge(line.b);
                            }
                            SVG svg(debug_out_path("make_slab_loops-in-%d-%d-%s.svg", iRun, line_idx, ProjectionFromTop ? "top" : "bottom").c_str(), bbox_svg);
                            svg.arrows = true;
                            for (const IntersectionLine& line : in) {
                                const char* color = line.source == IntersectionLine::Source::BottomPlane ? "red" : line.source == IntersectionLine::Source::TopPlane ? "blue" : "green";
                                svg.draw(line, color, stroke_width);
                            }
                            svg.Close();
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                        Polygons &loops = layers[line_idx];
                        std::vector<OpenPolyline> open_polylines;
                        chain_lines_by_triangle_connectivity(in, loops, open_polylines);
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
                        {
                            SVG svg(debug_out_path("make_slab_loops-out-%d-%d-%s.svg", iRun, line_idx, ProjectionFromTop ? "top" : "bottom").c_str(), bbox_svg);
                            svg.arrows = true;
                            for (const IntersectionLine& line : in) {
                                const char* color = line.source == IntersectionLine::Source::BottomPlane ? "red" : line.source == IntersectionLine::Source::TopPlane ? "blue" : "green";
                                svg.draw(line, color, stroke_width);
                            }
                            svg.draw(loops, "black");
                            svg.Close();
                        }
                        {
                            SVG svg(debug_out_path("make_slab_loops-open-polylines-%d-%d-%s.svg", iRun, line_idx, ProjectionFromTop ? "top" : "bottom").c_str(), bbox_svg);
                            svg.draw(loops, "black");
                            svg.arrows = true;
                            for (const OpenPolyline &open_polyline : open_polylines)
                                svg.draw(Polyline(open_polyline.points), "black", stroke_width);
                            svg.Close();
                        }
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
                        assert(! loops.empty());
                        assert(open_polylines.empty());
                        if (! open_polylines.empty())
                            BOOST_LOG_TRIVIAL(trace) << "make_slab_loops - chaining failed. #" << open_polylines.size() << " open polylines";
                    }
                }
            }
        }
    );

    return layers;
}

// Used to cut the mesh into two halves.
static ExPolygons make_expolygons_simple(std::vector<IntersectionLine> &lines)
{
    ExPolygons slices;
    Polygons holes;

    for (Polygon &loop : make_loops(lines))
        if (loop.area() >= 0.)
            slices.emplace_back(std::move(loop));
        else
            holes.emplace_back(std::move(loop));

    // If there are holes, then there should also be outer contours.
    assert(holes.empty() || ! slices.empty());
    if (! slices.empty())
    {
        // Assign holes to outer contours.
        for (Polygon &hole : holes) {
            // Find an outer contour to a hole.
            int     slice_idx            = -1;
            double  current_contour_area = std::numeric_limits<double>::max();
            for (ExPolygon &slice : slices)
                if (slice.contour.contains(hole.points.front())) {
                    double area = slice.contour.area();
                    if (area < current_contour_area) {
                        slice_idx = &slice - slices.data();
                        current_contour_area = area;
                    }
                }
            // assert(slice_idx != -1);
            if (slice_idx == -1)
                // Ignore this hole.
                continue;
            assert(current_contour_area < std::numeric_limits<double>::max() && current_contour_area >= -hole.area());
            slices[slice_idx].holes.emplace_back(std::move(hole));
        }

#if 0
        // If the input mesh is not valid, the holes may intersect with the external contour.
        // Rather subtract them from the outer contour.
        Polygons poly;
        for (auto it_slice = slices->begin(); it_slice != slices->end(); ++ it_slice) {
            if (it_slice->holes.empty()) {
                poly.emplace_back(std::move(it_slice->contour));
            } else {
                Polygons contours;
                contours.emplace_back(std::move(it_slice->contour));
                for (auto it = it_slice->holes.begin(); it != it_slice->holes.end(); ++ it)
                    it->reverse();
                polygons_append(poly, diff(contours, it_slice->holes));
            }
        }
        // If the input mesh is not valid, the input contours may intersect.
        *slices = union_ex(poly);
#endif

#if 0
        // If the input mesh is not valid, the holes may intersect with the external contour.
        // Rather subtract them from the outer contour.
        ExPolygons poly;
        for (auto it_slice = slices->begin(); it_slice != slices->end(); ++ it_slice) {
            Polygons contours;
            contours.emplace_back(std::move(it_slice->contour));
            for (auto it = it_slice->holes.begin(); it != it_slice->holes.end(); ++ it)
                it->reverse();
            expolygons_append(poly, diff_ex(contours, it_slice->holes));
        }
        // If the input mesh is not valid, the input contours may intersect.
        *slices = std::move(poly);
#endif
    }

    return slices;
}

static void make_expolygons(const Polygons &loops, const float closing_radius, const float extra_offset, ClipperLib::PolyFillType fill_type, ExPolygons* slices)
{
    /*
        Input loops are not suitable for evenodd nor nonzero fill types, as we might get
        two consecutive concentric loops having the same winding order - and we have to 
        respect such order. In that case, evenodd would create wrong inversions, and nonzero
        would ignore holes inside two concentric contours.
        So we're ordering loops and collapse consecutive concentric loops having the same 
        winding order.
        TODO: find a faster algorithm for this, maybe with some sort of binary search.
        If we computed a "nesting tree" we could also just remove the consecutive loops
        having the same winding order, and remove the extra one(s) so that we could just
        supply everything to offset() instead of performing several union/diff calls.
    
        we sort by area assuming that the outermost loops have larger area;
        the previous sorting method, based on $b->contains($a->[0]), failed to nest
        loops correctly in some edge cases when original model had overlapping facets
    */

    /* The following lines are commented out because they can generate wrong polygons,
       see for example issue #661 */

    //std::vector<double> area;
    //std::vector<size_t> sorted_area;  // vector of indices
    //for (Polygons::const_iterator loop = loops.begin(); loop != loops.end(); ++ loop) {
    //    area.emplace_back(loop->area());
    //    sorted_area.emplace_back(loop - loops.begin());
    //}
    //
    //// outer first
    //std::sort(sorted_area.begin(), sorted_area.end(),
    //    [&area](size_t a, size_t b) { return std::abs(area[a]) > std::abs(area[b]); });

    //// we don't perform a safety offset now because it might reverse cw loops
    //Polygons p_slices;
    //for (std::vector<size_t>::const_iterator loop_idx = sorted_area.begin(); loop_idx != sorted_area.end(); ++ loop_idx) {
    //    /* we rely on the already computed area to determine the winding order
    //       of the loops, since the Orientation() function provided by Clipper
    //       would do the same, thus repeating the calculation */
    //    Polygons::const_iterator loop = loops.begin() + *loop_idx;
    //    if (area[*loop_idx] > +EPSILON)
    //        p_slices.emplace_back(*loop);
    //    else if (area[*loop_idx] < -EPSILON)
    //        //FIXME This is arbitrary and possibly very slow.
    //        // If the hole is inside a polygon, then there is no need to diff.
    //        // If the hole intersects a polygon boundary, then diff it, but then
    //        // there is no guarantee of an ordering of the loops.
    //        // Maybe we can test for the intersection before running the expensive diff algorithm?
    //        p_slices = diff(p_slices, *loop);
    //}

    // Perform a safety offset to merge very close facets (TODO: find test case for this)
    // 0.0499 comes from https://github.com/slic3r/Slic3r/issues/959
//    double safety_offset = scale_(0.0499);
    // 0.0001 is set to satisfy GH #520, #1029, #1364
    assert(closing_radius >= 0);
    // Allowing negative extra_offset for shrinking a contour. This likely only makes sense if slicing a single region only.
    //assert(extra_offset >= 0);
    double offset_out;
    double offset_in;
    if (closing_radius >= extra_offset) {
        offset_out = + scale_(closing_radius);
        offset_in  = - scale_(closing_radius - extra_offset);
    } else {
        offset_out = + scale_(extra_offset);
        offset_in  = 0.;
    }

    /* The following line is commented out because it can generate wrong polygons,
       see for example issue #661 */
    //ExPolygons ex_slices = closing(p_slices, safety_offset);
    
    #ifdef SLIC3R_TRIANGLEMESH_DEBUG
    size_t holes_count = 0;
    for (ExPolygons::const_iterator e = ex_slices.begin(); e != ex_slices.end(); ++ e)
        holes_count += e->holes.size();
    printf("%zu surface(s) having %zu holes detected from %zu polylines\n",
        ex_slices.size(), holes_count, loops.size());
    #endif
    
    // append to the supplied collection
    expolygons_append(*slices,
        offset_out > 0 && offset_in < 0 ? offset2_ex(union_ex(loops, fill_type), offset_out, offset_in) :
        offset_out > 0 ? offset_ex(union_ex(loops, fill_type), offset_out) :
        offset_in  < 0 ? offset_ex(union_ex(loops, fill_type), offset_in) :
        union_ex(loops, fill_type));
}

// Make a trafo for transforming the vertices. Scale up in XY, not in Z.
static inline Transform3f make_trafo_for_slicing(const Transform3d &trafo)
{
    auto t = trafo;
    const double s = 1. / SCALING_FACTOR;
    t.prescale(Vec3d(s, s, 1.));
    return t.cast<float>();
}

static inline bool is_identity(const Transform3d &trafo)
{
    return trafo.matrix() == Transform3d::Identity().matrix();
}

static std::vector<stl_vertex> transform_mesh_vertices_for_slicing(const indexed_triangle_set &mesh, const Transform3d &trafo)
{
    // Copy and scale vertices in XY, don't scale in Z.
    // Possibly apply the transformation.
    const double   s = 1. / SCALING_FACTOR;
    std::vector<stl_vertex>         out(mesh.vertices);
    if (is_identity(trafo)) {
        // Identity.
        for (stl_vertex &v : out) {
            // Scale just XY, leave Z unscaled.
            v.x() *= float(s);
            v.y() *= float(s);
        }
    } else {
        // Transform the vertices, scale up in XY, not in Y.
        auto t = trafo;
        t.prescale(Vec3d(s, s, 1.));
        auto tf = t.cast<float>();
        for (stl_vertex &v : out)
            v = tf * v;
    }
    return out;
}

std::vector<Polygons> slice_mesh(
    const indexed_triangle_set       &mesh,
    // Unscaled Zs
    const std::vector<float>         &zs,
    const MeshSlicingParams          &params,
    std::function<void()>             throw_on_cancel)
{
    BOOST_LOG_TRIVIAL(debug) << "slice_mesh to polygons";
       
    std::vector<IntersectionLines> lines;

    {
        //FIXME facets_edges is likely not needed and quite costly to calculate.
        // Instead of edge identifiers, one shall use a sorted pair of edge vertex indices.
        // However facets_edges assigns a single edge ID to two triangles only, thus when factoring facets_edges out, one will have
        // to make sure that no code relies on it.
        std::vector<Vec3i32> face_edge_ids = its_face_edge_ids(mesh);
        if (zs.size() <= 1) {
            // It likely is not worthwile to copy the vertices. Apply the transformation in place.
            if (is_identity(params.trafo)) {
                lines = slice_make_lines(
                    mesh.vertices, [](const Vec3f &p) { return Vec3f(scaled<float>(p.x()), scaled<float>(p.y()), p.z()); }, 
                    mesh.indices, face_edge_ids, zs, throw_on_cancel);
            } else {
                // Transform the vertices, scale up in XY, not in Z.
                Transform3f tf = make_trafo_for_slicing(params.trafo);
                lines = slice_make_lines(mesh.vertices, [tf](const Vec3f &p) { return tf * p; }, mesh.indices, face_edge_ids, zs, throw_on_cancel);
            }
        } else {
            // Copy and scale vertices in XY, don't scale in Z. Possibly apply the transformation.
            lines = slice_make_lines(
                transform_mesh_vertices_for_slicing(mesh, params.trafo), 
                [](const Vec3f &p) { return p; },  mesh.indices, face_edge_ids, zs, throw_on_cancel);
        }
    }

    throw_on_cancel();

    std::vector<Polygons> layers = make_loops(lines, params, throw_on_cancel);

#ifdef SLIC3R_DEBUG
    {
        static int iRun = 0;
        for (size_t i = 0; i < z.size(); ++ i) {
            Polygons  &polygons   = (*layers)[i];
            ExPolygons expolygons = union_ex(polygons, true);
            SVG::export_expolygons(debug_out_path("slice_%d_%d.svg", iRun, i).c_str(), expolygons);
            {
                BoundingBox bbox;
                for (const IntersectionLine &l : lines[i]) {
                    bbox.merge(l.a);
                    bbox.merge(l.b);
                }
                SVG svg(debug_out_path("slice_loops_%d_%d.svg", iRun, i).c_str(), bbox);
                svg.draw(expolygons);
                for (const IntersectionLine &l : lines[i])
                    svg.draw(l, "red", 0);
                svg.draw_outline(expolygons, "black", "blue", 0);
                svg.Close();
            }
#if 0
//FIXME slice_facet() may create zero length edges due to rounding of doubles into coord_t.
            for (Polygon &poly : polygons) {
                for (size_t i = 1; i < poly.points.size(); ++ i)
                    assert(poly.points[i-1] != poly.points[i]);
                assert(poly.points.front() != poly.points.back());
            }
#endif
        }
        ++ iRun;
    }
#endif

    return layers;
}

// Specialized version for a single slicing plane only, running on a single thread.
Polygons slice_mesh(
    const indexed_triangle_set       &mesh,
    // Unscaled Zs
    const float                       plane_z,
    const MeshSlicingParams          &params)
{
    std::vector<IntersectionLines> lines;

    {
        bool                trafo_identity = is_identity(params.trafo);
        Transform3f         tf;
        std::vector<bool>   face_mask(mesh.indices.size(), false);

        {
            // 1) Mark vertices as below or above the slicing plane.
            std::vector<char> vertex_side(mesh.vertices.size(), 0);
            if (trafo_identity) {
                for (size_t i = 0; i < mesh.vertices.size(); ++ i) {
                    float z = mesh.vertices[i].z();
                    char  s = z < plane_z ? -1 : z == plane_z ? 0 : 1;
                    vertex_side[i] = s;
                }
            } else {
                tf = make_trafo_for_slicing(params.trafo);
                for (size_t i = 0; i < mesh.vertices.size(); ++ i) {
                    //FIXME don't need to transform x & y, just Z.
                    float z = (tf * mesh.vertices[i]).z();
                    char  s = z < plane_z ? -1 : z == plane_z ? 0 : 1;
                    vertex_side[i] = s;
                }
            }

            // 2) Mark faces crossing the plane.
            for (size_t i = 0; i < mesh.indices.size(); ++ i) {
                const Vec3i32 &face = mesh.indices[i];
                int sides[3] = { vertex_side[face(0)], vertex_side[face(1)], vertex_side[face(2)] };
                face_mask[i] = sides[0] * sides[1] <= 0 || sides[1] * sides[2] <= 0 || sides[0] * sides[2] <= 0;
            }
        }

        // 3) Calculate face neighbors for just the faces in face_mask.
        std::vector<Vec3i32> face_edge_ids = its_face_edge_ids(mesh, face_mask);

        // 4) Slice "face_mask" triangles, collect line segments.
        // It likely is not worthwile to copy the vertices. Apply the transformation in place.
        if (trafo_identity) {
            lines.emplace_back(slice_make_lines(
                mesh.vertices, [](const Vec3f &p) { return Vec3f(scaled<float>(p.x()), scaled<float>(p.y()), p.z()); }, 
                mesh.indices, face_edge_ids, plane_z, [&face_mask](int face_idx) { return face_mask[face_idx]; }));
        } else {
            // Transform the vertices, scale up in XY, not in Z.
            lines.emplace_back(slice_make_lines(mesh.vertices, [tf](const Vec3f& p) { return tf * p; }, mesh.indices, face_edge_ids, plane_z,
                [&face_mask](int face_idx) { return face_mask[face_idx]; }));
        }
    }

    // 5) Chain the line segments.
    std::vector<Polygons> layers = make_loops(lines, params, [](){});
    assert(layers.size() == 1);
    return layers.front();
}

std::vector<ExPolygons> slice_mesh_ex(
    const indexed_triangle_set       &mesh,
    const std::vector<float>         &zs,
    const MeshSlicingParamsEx        &params,
    std::function<void()>             throw_on_cancel)
{
    std::vector<Polygons> layers_p;
    {
        MeshSlicingParams slicing_params(params);
        if (params.mode == MeshSlicingParams::SlicingMode::PositiveLargestContour)
            slicing_params.mode = MeshSlicingParams::SlicingMode::Positive;
        if (params.mode_below == MeshSlicingParams::SlicingMode::PositiveLargestContour)
            slicing_params.mode_below = MeshSlicingParams::SlicingMode::Positive;
        layers_p = slice_mesh(mesh, zs, slicing_params, throw_on_cancel);
    }
    
//    BOOST_LOG_TRIVIAL(debug) << "slice_mesh make_expolygons in parallel - start";
    std::vector<ExPolygons> layers(layers_p.size(), ExPolygons{});
    tbb::parallel_for(
        tbb::blocked_range<size_t>(0, layers_p.size()),
        [&layers_p, &params, &layers, throw_on_cancel]
        (const tbb::blocked_range<size_t>& range) {
            auto resolution = scaled<float>(params.resolution);
            for (size_t layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
                throw_on_cancel();
                ExPolygons &expolygons = layers[layer_id];
                const auto this_mode = layer_id < params.slicing_mode_normal_below_layer ? params.mode_below : params.mode;
                Slic3r::make_expolygons(
                    layers_p[layer_id], params.closing_radius, params.extra_offset,
                    this_mode == MeshSlicingParams::SlicingMode::EvenOdd ? ClipperLib::pftEvenOdd : 
                    this_mode == MeshSlicingParams::SlicingMode::PositiveLargestContour ? ClipperLib::pftPositive : ClipperLib::pftNonZero,
                    &expolygons);
                //FIXME simplify
                if (this_mode == MeshSlicingParams::SlicingMode::PositiveLargestContour)
                    keep_largest_contour_only(expolygons);
                if (resolution != 0.) {
                    ExPolygons simplified;
                    simplified.reserve(expolygons.size());
                    for (const ExPolygon &ex : expolygons)
                        append(simplified, ex.simplify(resolution));
                    expolygons = std::move(simplified);
                }
            }
        });
//    BOOST_LOG_TRIVIAL(debug) << "slice_mesh make_expolygons in parallel - end";

    return layers;
}

// Slice a triangle set with a set of Z slabs (thick layers).
// The effect is similar to producing the usual top / bottom layers from a sliced mesh by 
// subtracting layer[i] from layer[i - 1] for the top surfaces resp.
// subtracting layer[i] from layer[i + 1] for the bottom surfaces,
// with the exception that the triangle set this function processes may not cover the whole top resp. bottom surface.
// top resp. bottom surfaces are calculated only if out_top resp. out_bottom is not null.
void slice_mesh_slabs(
    const indexed_triangle_set       &mesh,
    // Unscaled Zs
    const std::vector<float>         &zs,
    const Transform3d                &trafo,
    std::vector<Polygons>            *out_top,
    std::vector<Polygons>            *out_bottom,
    std::vector<std::pair<Vec3f, Vec3f>>   *vertical_points,
    std::function<void()>             throw_on_cancel)
{
    BOOST_LOG_TRIVIAL(debug) << "slice_mesh_slabs to polygons";

#ifdef EXPENSIVE_DEBUG_CHECKS
    {
        // Verify that the vertices are unique.
        auto v = mesh.vertices;
        std::sort(v.begin(), v.end(), [](auto &l, auto &r) {
                return l.x() < r.x() || (l.x() == r.x() && (l.y() < r.y() || (l.y() == r.y() && l.z() < r.z())));
            });
        size_t num_duplicates = v.end() - std::unique(v.begin(), v.end());
        assert(num_duplicates == 0);
    }
    if (0)
    {
        // Verify that there are no T-joints.
        // The T-joints could likely be already part of the source mesh.
        for (const auto &tri : mesh.indices)
            for (int i = 0; i < 3; ++ i) {
                int j = next_idx_modulo(i, 3);
                int k = next_idx_modulo(j, 3);
                auto &v1 = mesh.vertices[tri(i)];
                auto &v2 = mesh.vertices[tri(j)];
                auto &v3 = mesh.vertices[tri(k)];
                for (auto &pt : mesh.vertices)
                    if (&pt != &v1 && &pt != &v2) {
                        assert(pt != v1 && pt != v2);
                        assert((pt - v1).norm() > EPSILON);
                        assert((pt - v2).norm() > EPSILON);
                        auto   l2  = (v2 - v1).squaredNorm();
                        assert(l2 > 0);
                        auto   t   = (pt - v1).dot(v2 - v1);
                        if (t > 0 && t < l2) {
                            auto d2 = (pt - v1).squaredNorm() - sqr(t) / l2;
                            auto d = sqrt(std::max(d2, 0.f));
                            if (&pt == &v3) {
                                if (d < EPSILON)
                                    printf("Degenerate triangle!\n");
                            } else {
                                assert(d > EPSILON);
                            }
                        }
                    }
            }
    }
#endif // EXPENSIVE_DEBUG_CHECKS

    std::vector<stl_vertex> vertices_transformed = transform_mesh_vertices_for_slicing(mesh, trafo);
    const auto mirrored_sign = int64_t(trafo.matrix().block(0, 0, 3, 3).determinant() < 0 ? -1 : 1);

    std::vector<FaceOrientation> face_orientation(mesh.indices.size(), FaceOrientation::Up);
    for (const stl_triangle_vertex_indices &tri : mesh.indices) {
        const Vec3f   fa = vertices_transformed[tri(0)];
        const Vec3f   fb = vertices_transformed[tri(1)];
        const Vec3f   fc = vertices_transformed[tri(2)];
        assert(fa != fb && fa != fc && fb != fc);
        const Point   a = to_2d(fa).cast<coord_t>();
        const Point   b = to_2d(fb).cast<coord_t>();
        const Point   c = to_2d(fc).cast<coord_t>();
        const int64_t d = cross2((b - a).cast<int64_t>(), (c - b).cast<int64_t>()) * mirrored_sign;
        FaceOrientation fo = FaceOrientation::Vertical;
        if (d > 0)
            fo = FaceOrientation::Up;
        else if (d < 0)
            fo = FaceOrientation::Down;
        else {
            // Is the triangle vertical or degenerate?
            assert(d == 0);
            fo = fa == fb || fa == fc || fb == fc ? FaceOrientation::Degenerate : FaceOrientation::Vertical;
            if(vertical_points && fo==FaceOrientation::Vertical)
            {
                Vec3f normal = (fb - fa).cross(fc - fa).normalized();
                vertical_points->push_back({ (fa + fb + fc) / 3,normal });
            }
        }
        face_orientation[&tri - mesh.indices.data()] = fo;
    }

    std::vector<Vec3i32> face_neighbors = its_face_neighbors_par(mesh);
    int                num_edges;
    std::vector<Vec3i32> face_edge_ids  = its_face_edge_ids(mesh, face_neighbors, true, &num_edges);
    std::pair<SlabLines, SlabLines> lines = slice_slabs_make_lines(
        vertices_transformed, mesh.indices, face_neighbors, face_edge_ids, num_edges, face_orientation, zs, 
        out_top != nullptr, out_bottom != nullptr, throw_on_cancel);

    throw_on_cancel();

    if (out_top)
        *out_top = make_slab_loops<true>(lines.first, num_edges, throw_on_cancel);
    if (out_bottom)
        *out_bottom = make_slab_loops<false>(lines.second, num_edges, throw_on_cancel);
}

// Remove duplicates of slice_vertices, optionally triangulate the cut.
static void triangulate_slice(
    indexed_triangle_set    &its,
    IntersectionLines       &lines,
    std::vector<int>        &slice_vertices,
    // Vertices of the original (unsliced) mesh. Newly added vertices are those on the slice.
    int                      num_original_vertices,
    // Z height of the slice.
    float                    z,
    bool                     triangulate,
    bool                     normals_down,
    const std::map<int, Vec3f> &section_vertices_map)
{
    sort_remove_duplicates(slice_vertices);

    // 1) Create map of the slice vertices from positions to mesh indices.
    // As the caller will likely add duplicate points when intersecting triangle edges, there will be duplicates.
    std::vector<std::pair<Vec2f, int>> map_vertex_to_index;
    map_vertex_to_index.reserve(slice_vertices.size());
    for (int i : slice_vertices)
        map_vertex_to_index.emplace_back(to_2d(its.vertices[i]), i);
    std::sort(map_vertex_to_index.begin(), map_vertex_to_index.end(),
        [](const std::pair<Vec2f, int> &l, const std::pair<Vec2f, int> &r) {
            return l.first.x() < r.first.x() || 
                   (is_equal_for_sort(l.first.x(), r.first.x()) && (l.first.y()< r.first.y() || 
                                                          (is_equal_for_sort(l.first.y(), r.first.y()) && l.second < r.second)));
    });

    // 2) Discover duplicate points on the slice. Remap duplicate vertices to a vertex with a lowest index.
    //    Remove denegerate triangles, if they happen to be created by merging duplicate vertices.
    {
        std::vector<int> map_duplicate_vertex(int(its.vertices.size()) - num_original_vertices, -1);
        int i = 0;
        int k = 0;
        for (; i < int(map_vertex_to_index.size());) {
            map_vertex_to_index[k ++] = map_vertex_to_index[i];
            const Vec2f &ipos = map_vertex_to_index[i].first;
            const int    iidx = map_vertex_to_index[i].second;
            if (iidx >= num_original_vertices)
                // map to itself
                map_duplicate_vertex[iidx - num_original_vertices] = iidx;
            int j = i;
            for (++ j; j < int(map_vertex_to_index.size()) && is_equal(ipos.x(), map_vertex_to_index[j].first.x()) && is_equal(ipos.y(), map_vertex_to_index[j].first.y()); ++ j) {
                const int jidx = map_vertex_to_index[j].second;
                assert(jidx >= num_original_vertices);
                if (jidx >= num_original_vertices)
                    // map to the first vertex
                    map_duplicate_vertex[jidx - num_original_vertices] = iidx;
            }
            i = j;
        }
        map_vertex_to_index.erase(map_vertex_to_index.begin() + k, map_vertex_to_index.end());
        for (i = 0; i < int(its.indices.size());) {
            stl_triangle_vertex_indices &f = its.indices[i];
            // Remap the newly added face vertices.
            for (k = 0; k < 3; ++ k)
                if (f(k) >= num_original_vertices)
                    f(k) = map_duplicate_vertex[f(k) - num_original_vertices];
            if (f(0) == f(1) || f(0) == f(2) || f(1) == f(2)) {
                // Remove degenerate face.
                f = its.indices.back();
                its.indices.pop_back();
            } else
                // Keep the face.
                ++ i;
        }
    }

    if (triangulate) {
        size_t idx_vertex_new_first = its.vertices.size();
        Pointf3s triangles = triangulate_expolygons_3d(make_expolygons_simple(lines), z, normals_down);
        for (size_t i = 0; i < triangles.size(); ) {
            stl_triangle_vertex_indices facet;
            for (size_t j = 0; j < 3; ++ j) {
                Vec3f v = triangles[i ++].cast<float>();
                auto it = lower_bound_by_predicate(map_vertex_to_index.begin(), map_vertex_to_index.end(),
                    [&v](const std::pair<Vec2f, int> &l) {
                    return l.first.x() < v.x() || (is_equal_for_sort(l.first.x(), v.x()) && l.first.y() < v.y());
                    });
                auto  back_it = it;
                int   idx = -1;
                bool exist = false;
                for (auto iter = section_vertices_map.begin(); iter != section_vertices_map.end(); iter++) {
                    if (is_equal(v, iter->second)) {
                        idx   = iter->first;
                        exist = true;
                        break;
                    }
                }
                // go on finding
                if (!exist) {
                    for (; it != map_vertex_to_index.end(); it++) {
                        if (is_equal(it->first.x(), v.x()) && is_equal(it->first.y(), v.y())) {
                            idx   = it->second;
                            exist = true;
                            break;
                        }
                    }
                }
                // go on finding
                if (!exist) {
                    it = back_it;
                    for (; it != map_vertex_to_index.begin(); it--) {
                        if (is_equal(it->first.x(), v.x()) && is_equal(it->first.y(), v.y())) {
                            idx   = it->second;
                            exist = true;
                            break;
                        }
                    }
                }
                if (!exist){
                    // Try to find the vertex in the list of newly added vertices. Those vertices are not matched on the cut and they shall be rare.
                    for (size_t k = idx_vertex_new_first; k < its.vertices.size(); ++ k)
                        if (its.vertices[k] == v) {
                            idx = int(k);
                            break;
                        }
                    if (idx == -1) {
                        idx = int(its.vertices.size());
                        its.vertices.emplace_back(v);
                    }
                }
                facet(j) = idx;
            }
            if (facet(0) != facet(1) && facet(0) != facet(2) && facet(1) != facet(2))
                its.indices.emplace_back(facet);
        }
    }

    // Remove vertices, which are not referenced by any face.
    its_compactify_vertices(its);

    // Degenerate faces should not be created.
    // its_remove_degenerate_faces(its);
}

void project_mesh(
    const indexed_triangle_set       &mesh,
    const Transform3d                &trafo,
    Polygons                         *out_top,
    Polygons                         *out_bottom,
    std::function<void()>             throw_on_cancel)
{
    std::vector<Polygons> top, bottom;
    std::vector<float>    zs { -1e10, 1e10 };
    slice_mesh_slabs(mesh, zs, trafo, out_top ? &top : nullptr, out_bottom ? &bottom : nullptr, nullptr, throw_on_cancel);
    if (out_top)
        *out_top = std::move(top.front());
    if (out_bottom)
        *out_bottom = std::move(bottom.back());
}

Polygons project_mesh(
    const indexed_triangle_set       &mesh,
    const Transform3d                &trafo,
    std::function<void()>             throw_on_cancel)
{
    std::vector<Polygons> top, bottom;
    std::vector<float>    zs { -1e10, 1e10 };
    slice_mesh_slabs(mesh, zs, trafo, &top, &bottom, nullptr, throw_on_cancel);
    return union_(top.front(), bottom.back());
}

void cut_mesh(const indexed_triangle_set& mesh, float z, indexed_triangle_set* upper, indexed_triangle_set* lower, bool triangulate_caps)
{
    assert(upper || lower);
    if (upper == nullptr && lower == nullptr)
        return;

    BOOST_LOG_TRIVIAL(trace) << "cut_mesh - slicing object";

    if (upper) {
        upper->clear();
        upper->vertices = mesh.vertices;
        upper->indices.reserve(mesh.indices.size());
    }

    if (lower) {
        lower->clear();
        lower->vertices = mesh.vertices;
        lower->indices.reserve(mesh.indices.size());
    }

#ifndef NDEBUG
    size_t num_open_edges_old = triangulate_caps ? its_num_open_edges(mesh) : 0;
#endif // NDEBUG

    // To triangulate the caps after slicing.
    IntersectionLines  upper_lines, lower_lines;
    std::vector<int>   upper_slice_vertices, lower_slice_vertices;
    std::vector<Vec3i32> facets_edge_ids = its_face_edge_ids(mesh);
    std::map<int, Vec3f> section_vertices_map;

    for (int facet_idx = 0; facet_idx < int(mesh.indices.size()); ++ facet_idx) {
        const stl_triangle_vertex_indices &facet = mesh.indices[facet_idx];
        Vec3f vertices[3] { mesh.vertices[facet(0)], mesh.vertices[facet(1)], mesh.vertices[facet(2)] };
        float min_z = std::min(vertices[0].z(), std::min(vertices[1].z(), vertices[2].z()));
        float max_z = std::max(vertices[0].z(), std::max(vertices[1].z(), vertices[2].z()));

        for (size_t i = 0; i < 3; i++) {
            if (is_equal(z, vertices[i].z()) && section_vertices_map.find(facet(i)) == section_vertices_map.end()) {
                section_vertices_map.emplace(facet(i), vertices[i]);
            }
        }
        // intersect facet with cutting plane
        IntersectionLine line;
        int              idx_vertex_lowest = is_equal(vertices[1].z(), min_z) ? 1 : (is_equal(vertices[2].z() , min_z) ? 2 : 0);
        FacetSliceType   slice_type = FacetSliceType::NoSlice;
        if (z > min_z - EPSILON && z < max_z + EPSILON) {
            Vec3f vertices_scaled[3];
            for (int i = 0; i < 3; ++ i) {
                const Vec3f &src = vertices[i];
                Vec3f       &dst = vertices_scaled[i];
                dst.x() = scale_(src.x());
                dst.y() = scale_(src.y());
                dst.z() = src.z();
            }
            slice_type = slice_facet_for_cut_mesh(z, vertices_scaled, mesh.indices[facet_idx], facets_edge_ids[facet_idx], idx_vertex_lowest, is_equal(min_z, max_z), line);
        }

        if (slice_type != FacetSliceType::NoSlice) {
            // Save intersection lines for generating correct triangulations.
            if (line.edge_type == IntersectionLine::FacetEdgeType::Top) {
                lower_lines.emplace_back(line);
                lower_slice_vertices.emplace_back(line.a_id);
                lower_slice_vertices.emplace_back(line.b_id);
            } else if (line.edge_type == IntersectionLine::FacetEdgeType::Bottom) {
                upper_lines.emplace_back(line);
                upper_slice_vertices.emplace_back(line.a_id);
                upper_slice_vertices.emplace_back(line.b_id);
            } else if (line.edge_type == IntersectionLine::FacetEdgeType::General) {
                lower_lines.emplace_back(line);
                upper_lines.emplace_back(line);
            }
        }

        if (min_z > z || (is_equal(min_z , z) && max_z > z)) {
            // facet is above the cut plane and does not belong to it
            if (upper != nullptr)
                upper->indices.emplace_back(facet);
        } else if (max_z < z || (is_equal(max_z, z) && min_z < z)) {
            // facet is below the cut plane and does not belong to it
            if (lower != nullptr)
                lower->indices.emplace_back(facet);
        } else if (min_z < z && max_z > z) {
            // Facet is cut by the slicing plane.
            assert(slice_type == FacetSliceType::Slicing);
            assert(line.edge_type == IntersectionLine::FacetEdgeType::General);
            assert(line.edge_a_id != -1);
            assert(line.edge_b_id != -1);

            // look for the vertex on whose side of the slicing plane there are no other vertices
            int              isolated_vertex, isolated_vertex_option = -1;
            std::vector<int> list{0, 1, 2};
            auto             get_third = [&list](int isolated_vertex, int temp) {// not use vertex data
                for (size_t i = 0; i < list.size(); i++) {
                    if (list[i] != isolated_vertex && list[i] != temp) {
                        return list[i];
                    }
                }
                return -1;
            };
            if (is_equal(vertices[0].z(), z)) {
                isolated_vertex        = vertices[1].z() > z ? 1 : 2;
                isolated_vertex_option = get_third(isolated_vertex, 0);
            } else if (is_equal(vertices[1].z(), z)) {
                isolated_vertex        = vertices[2].z() > z ? 2 : 0;
                isolated_vertex_option = get_third(isolated_vertex, 1);
            } else if (is_equal(vertices[2].z(), z)) {
                isolated_vertex        = vertices[0].z() > z ? 0 : 1;
                isolated_vertex_option = get_third(isolated_vertex, 2);
            } else {
                isolated_vertex = (vertices[0].z() > z) == (vertices[1].z() > z) ? 2 : (vertices[1].z() > z) == (vertices[2].z() > z) ? 0 : 1;
            }
            // get vertices starting from the isolated one
            stl_vertex v0v1, v2v0;
            auto       calc_isolated_vertex = [&v0v1, &v2v0, &line, &facet_idx, &facets_edge_ids, &z](int iv, bool &is_find) {
                assert(facets_edge_ids[facet_idx](iv) == line.edge_a_id || facets_edge_ids[facet_idx](iv) == line.edge_b_id);
                is_find = true;
                if (facets_edge_ids[facet_idx](iv) == line.edge_a_id) {
                    // Unscale to doubles first, then to floats to reach the same accuracy as triangulate_expolygons_2d().
                    v0v1 = to_3d(unscaled<double>(line.a).cast<float>().eval(), z);
                    v2v0 = to_3d(unscaled<double>(line.b).cast<float>().eval(), z);
                } else if (facets_edge_ids[facet_idx](iv) == line.edge_b_id) {
                    v0v1 = to_3d(unscaled<double>(line.b).cast<float>().eval(), z);
                    v2v0 = to_3d(unscaled<double>(line.a).cast<float>().eval(), z);
                } else {
                    is_find = false;
                }
            };
            bool find_isolated_vertex;
            int  iv;
            calc_isolated_vertex(isolated_vertex, find_isolated_vertex);
            if (!find_isolated_vertex && isolated_vertex_option != -1) {
                calc_isolated_vertex(isolated_vertex_option, find_isolated_vertex);
                if (!find_isolated_vertex) {
                    BOOST_LOG_TRIVIAL(trace) << "cut_mesh:error:could not find isolated_vertex";
                    continue;
                }
                iv = isolated_vertex_option;
            } else {
                iv = isolated_vertex;
            }
            const stl_vertex &v0  = vertices[iv];
            const int         iv0 = facet[iv];
            if (++ iv == 3)
                iv = 0;
            const stl_vertex &v1  = vertices[iv];
            const int         iv1 = facet[iv];
            if (++ iv == 3)
                iv = 0;
            const stl_vertex &v2  = vertices[iv];
            const int         iv2 = facet[iv];

            // intersect v0-v1 and v2-v0 with cutting plane and make new vertices
            auto new_vertex = [upper, lower, &upper_slice_vertices, &lower_slice_vertices](const Vec3f &a, const int ia, const Vec3f &b, const int ib, const Vec3f &c,
                                                                                           const int ic, const Vec3f &new_pt, bool &is_new_vertex) {
                int iupper = 0, ilower = 0;
                is_new_vertex = false;
                if (is_equal(new_pt, a))
                    iupper = ilower = ia;
                else if (is_equal(new_pt, b))
                    iupper = ilower = ib;
                else if (is_equal(new_pt, c))
                    iupper = ilower = ic;
                else {
                    // Insert a new vertex into upper / lower.
                    is_new_vertex = true;
                    if (upper) {
                        iupper = int(upper->vertices.size());
                        upper->vertices.emplace_back(new_pt);
                        upper_slice_vertices.emplace_back(iupper);
                    }
                    if (lower) {
                        ilower = int(lower->vertices.size());
                        lower->vertices.emplace_back(new_pt);
                        lower_slice_vertices.emplace_back(ilower);
                    }
                }
                return std::make_pair(iupper, ilower);
            };
            bool is_new_vertex_v0v1;
            bool is_new_vertex_v2v0;
            auto [iv0v1_upper, iv0v1_lower] = new_vertex(v1, iv1, v0, iv0, v2, iv2, v0v1, is_new_vertex_v0v1);
            auto [iv2v0_upper, iv2v0_lower] = new_vertex(v2, iv2, v0, iv0, v1, iv1, v2v0, is_new_vertex_v2v0);
            auto new_face                   = [](indexed_triangle_set *its, int i, int j, int k) {
                if (its != nullptr && i != j && i != k && j != k) its->indices.emplace_back(i, j, k);
            };
            if (is_new_vertex_v0v1 && is_new_vertex_v2v0) {
                if (v0.z() > z) {
                    new_face(upper, iv0, iv0v1_upper, iv2v0_upper);
                    new_face(lower, iv1, iv2, iv0v1_lower);
                    new_face(lower, iv2, iv2v0_lower, iv0v1_lower);
                } else {
                    new_face(upper, iv1, iv2, iv0v1_upper);
                    new_face(upper, iv2, iv2v0_upper, iv0v1_upper);
                    new_face(lower, iv0, iv0v1_lower, iv2v0_lower);
                }
            } else if (is_new_vertex_v0v1) {
                if (v0.z() > z) {
                    new_face(upper, iv0, iv0v1_upper, iv2);
                    new_face(lower, iv1, iv2, iv0v1_lower);
                } else {
                    new_face(lower, iv0, iv0v1_lower, iv2);
                    new_face(upper, iv1, iv2, iv0v1_upper);
                }
            } else if (is_new_vertex_v2v0) {
                if (v0.z() > z) {
                    new_face(upper, iv0, iv1, iv2v0_upper);
                    new_face(lower, iv1, iv2, iv2v0_lower);
                } else {
                    new_face(lower, iv0, iv1, iv2v0_lower);
                    new_face(upper, iv1, iv2, iv2v0_upper);
                }
            }
        }
    }

    if (upper != nullptr) {
        triangulate_slice(*upper, upper_lines, upper_slice_vertices, int(mesh.vertices.size()), z, triangulate_caps, NORMALS_DOWN, section_vertices_map);
#ifndef NDEBUG
        if (triangulate_caps) {
            size_t num_open_edges_new = its_num_open_edges(*upper);
            assert(num_open_edges_new <= num_open_edges_old);
        }
#endif // NDEBUG
    }

    if (lower != nullptr) {
        triangulate_slice(*lower, lower_lines, lower_slice_vertices, int(mesh.vertices.size()), z, triangulate_caps, NORMALS_UP, section_vertices_map);
#ifndef NDEBUG
        if (triangulate_caps) {
            size_t num_open_edges_new = its_num_open_edges(*lower);
            assert(num_open_edges_new <= num_open_edges_old);
        }
#endif // NDEBUG
    }
}

} // namespace Slic3r
