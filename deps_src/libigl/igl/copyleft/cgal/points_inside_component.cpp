// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "points_inside_component.h"
#include "../../LinSpaced.h"
#include "order_facets_around_edge.h"
#include "assign_scalar.h"

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

#include <cassert>
#include <list>
#include <limits>
#include <vector>


namespace igl {
  namespace copyleft 
  {
    namespace cgal {
        namespace points_inside_component_helper {
            typedef CGAL::Exact_predicates_exact_constructions_kernel Kernel;
            typedef Kernel::Ray_3 Ray_3;
            typedef Kernel::Point_3 Point_3;
            typedef Kernel::Vector_3 Vector_3;
            typedef Kernel::Triangle_3 Triangle;
            typedef Kernel::Plane_3 Plane_3;
            typedef std::vector<Triangle>::iterator Iterator;
            typedef CGAL::AABB_triangle_primitive<Kernel, Iterator> Primitive;
            typedef CGAL::AABB_traits<Kernel, Primitive> AABB_triangle_traits;
            typedef CGAL::AABB_tree<AABB_triangle_traits> Tree;

            template<typename DerivedF, typename DerivedI>
            void extract_adj_faces(
                    const Eigen::PlainObjectBase<DerivedF>& F,
                    const Eigen::PlainObjectBase<DerivedI>& I,
                    const size_t s, const size_t d,
                    std::vector<int>& adj_faces) {
                const size_t num_faces = I.rows();
                for (size_t i=0; i<num_faces; i++) {
                    Eigen::Vector3i f = F.row(I(i, 0));
                    if (((size_t)f[0] == s && (size_t)f[1] == d) ||
                        ((size_t)f[1] == s && (size_t)f[2] == d) ||
                        ((size_t)f[2] == s && (size_t)f[0] == d)) {
                        adj_faces.push_back((I(i, 0)+1) * -1);
                        continue;
                    }
                    if (((size_t)f[0] == d && (size_t)f[1] == s) ||
                        ((size_t)f[1] == d && (size_t)f[2] == s) ||
                        ((size_t)f[2] == d && (size_t)f[0] == s)) {
                        adj_faces.push_back(I(i, 0)+1);
                        continue;
                    }
                }
            }

            template<typename DerivedF, typename DerivedI>
            void extract_adj_vertices(
                    const Eigen::PlainObjectBase<DerivedF>& F,
                    const Eigen::PlainObjectBase<DerivedI>& I,
                    const size_t v, std::vector<int>& adj_vertices) {
                std::set<size_t> unique_adj_vertices;
                const size_t num_faces = I.rows();
                for (size_t i=0; i<num_faces; i++) {
                    Eigen::Vector3i f = F.row(I(i, 0));
                    if ((size_t)f[0] == v) {
                        unique_adj_vertices.insert(f[1]);
                        unique_adj_vertices.insert(f[2]);
                    } else if ((size_t)f[1] == v) {
                        unique_adj_vertices.insert(f[0]);
                        unique_adj_vertices.insert(f[2]);
                    } else if ((size_t)f[2] == v) {
                        unique_adj_vertices.insert(f[0]);
                        unique_adj_vertices.insert(f[1]);
                    }
                }
                adj_vertices.resize(unique_adj_vertices.size());
                std::copy(unique_adj_vertices.begin(),
                        unique_adj_vertices.end(),
                        adj_vertices.begin());
            }

            template<typename DerivedV, typename DerivedF, typename DerivedI>
            bool determine_point_edge_orientation(
                    const Eigen::PlainObjectBase<DerivedV>& V,
                    const Eigen::PlainObjectBase<DerivedF>& F,
                    const Eigen::PlainObjectBase<DerivedI>& I,
                    const Point_3& query, size_t s, size_t d) {
                // Algorithm:
                //
                // Order the adj faces around the edge (s,d) clockwise using
                // query point as pivot.  (i.e. The first face of the ordering
                // is directly after the pivot point, and the last face is
                // directly before the pivot.)
                //
                // The point is outside if the first and last faces of the
                // ordering forms a convex angle.  This check can be done
                // without any construction by looking at the orientation of the
                // faces.  The angle is convex iff the first face contains (s,d)
                // as an edge and the last face contains (d,s) as an edge.
                //
                // The point is inside if the first and last faces of the
                // ordering forms a concave angle.  That is the first face
                // contains (d,s) as an edge and the last face contains (s,d) as
                // an edge.
                //
                // In the special case of duplicated faces. I.e. multiple faces
                // sharing the same 3 corners, but not necessarily the same
                // orientation.  The ordering will always rank faces containing
                // edge (s,d) before faces containing edge (d,s).
                //
                // Therefore, if there are any duplicates of the first faces,
                // the ordering will always choose the one with edge (s,d) if
                // possible.  The same for the last face.
                //
                // In the very degenerated case where the first and last face
                // are duplicates, but with different orientations, it is
                // equally valid to think the angle formed by them is either 0
                // or 360 degrees.  By default, 0 degree is used, and thus the
                // query point is outside.

                std::vector<int> adj_faces;
                extract_adj_faces(F, I, s, d, adj_faces);
                const size_t num_adj_faces = adj_faces.size();
                assert(num_adj_faces > 0);

                DerivedV pivot_point(1, 3);
                igl::copyleft::cgal::assign_scalar(query.x(), pivot_point(0, 0));
                igl::copyleft::cgal::assign_scalar(query.y(), pivot_point(0, 1));
                igl::copyleft::cgal::assign_scalar(query.z(), pivot_point(0, 2));
                Eigen::VectorXi order;
                order_facets_around_edge(V, F, s, d,
                        adj_faces, pivot_point, order);
                assert((size_t)order.size() == num_adj_faces);
                if (adj_faces[order[0]] > 0 &&
                    adj_faces[order[num_adj_faces-1] < 0]) {
                    return true;
                } else if (adj_faces[order[0]] < 0 &&
                    adj_faces[order[num_adj_faces-1] > 0]) {
                    return false;
                } else {
                    throw "The input mesh does not represent a valid volume";
                }
                throw "The input mesh does not represent a valid volume";
                return false;
            }

            template<typename DerivedV, typename DerivedF, typename DerivedI>
            bool determine_point_vertex_orientation(
                    const Eigen::PlainObjectBase<DerivedV>& V,
                    const Eigen::PlainObjectBase<DerivedF>& F,
                    const Eigen::PlainObjectBase<DerivedI>& I,
                    const Point_3& query, size_t s) {
                std::vector<int> adj_vertices;
                extract_adj_vertices(F, I, s, adj_vertices);
                const size_t num_adj_vertices = adj_vertices.size();

                std::vector<Point_3> adj_points;
                for (size_t i=0; i<num_adj_vertices; i++) {
                    const size_t vi = adj_vertices[i];
                    adj_points.emplace_back(V(vi,0), V(vi,1), V(vi,2));
                }

                // A plane is on the exterior if all adj_points lies on or to
                // one side of the plane.
                auto is_on_exterior = [&](const Plane_3& separator) -> bool{
                    size_t positive=0;
                    size_t negative=0;
                    size_t coplanar=0;
                    for (const auto& point : adj_points) {
                        switch(separator.oriented_side(point)) {
                            case CGAL::ON_POSITIVE_SIDE:
                                positive++;
                                break;
                            case CGAL::ON_NEGATIVE_SIDE:
                                negative++;
                                break;
                            case CGAL::ON_ORIENTED_BOUNDARY:
                                coplanar++;
                                break;
                            default:
                                throw "Unknown plane-point orientation";
                        }
                    }
                    auto query_orientation = separator.oriented_side(query);
                    bool r =
                        (positive == 0 && query_orientation == CGAL::POSITIVE)
                        ||
                        (negative == 0 && query_orientation == CGAL::NEGATIVE);
                    return r;
                };

                size_t d = std::numeric_limits<size_t>::max();
                Point_3 p(V(s,0), V(s,1), V(s,2));
                for (size_t i=0; i<num_adj_vertices; i++) {
                    const size_t vi = adj_vertices[i];
                    for (size_t j=i+1; j<num_adj_vertices; j++) {
                        Plane_3 separator(p, adj_points[i], adj_points[j]);
                        if (separator.is_degenerate()) {
                            throw "Input mesh contains degenerated faces";
                        }
                        if (is_on_exterior(separator)) {
                            d = vi;
                            assert(!CGAL::collinear(p, adj_points[i], query));
                            break;
                        }
                    }
                    if (d < (size_t)V.rows()) break;
                }
                if (d > (size_t)V.rows()) {
                    // All adj faces are coplanar, use the first edge.
                    d = adj_vertices[0];
                }
                return determine_point_edge_orientation(V, F, I, query, s, d);
            }

            template<typename DerivedV, typename DerivedF, typename DerivedI>
            bool determine_point_face_orientation(
                    const Eigen::PlainObjectBase<DerivedV>& V,
                    const Eigen::PlainObjectBase<DerivedF>& F,
                    const Eigen::PlainObjectBase<DerivedI>& I,
                    const Point_3& query, size_t fid) {
                // Algorithm: A point is on the inside of a face if the
                // tetrahedron formed by them is negatively oriented.

                Eigen::Vector3i f = F.row(I(fid, 0));
                const Point_3 v0(V(f[0], 0), V(f[0], 1), V(f[0], 2));
                const Point_3 v1(V(f[1], 0), V(f[1], 1), V(f[1], 2));
                const Point_3 v2(V(f[2], 0), V(f[2], 1), V(f[2], 2));
                auto result = CGAL::orientation(v0, v1, v2, query);
                if (result == CGAL::COPLANAR) {
                    throw "Cannot determine inside/outside because query point lies exactly on the input surface.";
                }
                return result == CGAL::NEGATIVE;
            }
        }
    }
  }
}

template<typename DerivedV, typename DerivedF, typename DerivedI,
    typename DerivedP, typename DerivedB>
IGL_INLINE void igl::copyleft::cgal::points_inside_component(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedI>& I,
        const Eigen::PlainObjectBase<DerivedP>& P,
        Eigen::PlainObjectBase<DerivedB>& inside) {
    using namespace igl::copyleft::cgal::points_inside_component_helper;
    if (F.rows() <= 0 || I.rows() <= 0) {
        throw "Inside check cannot be done on empty facet component.";
    }

    const size_t num_faces = I.rows();
    std::vector<Triangle> triangles;
    for (size_t i=0; i<num_faces; i++) {
        const Eigen::Vector3i f = F.row(I(i, 0));
        triangles.emplace_back(
                Point_3(V(f[0], 0), V(f[0], 1), V(f[0], 2)),
                Point_3(V(f[1], 0), V(f[1], 1), V(f[1], 2)),
                Point_3(V(f[2], 0), V(f[2], 1), V(f[2], 2)));
        if (triangles.back().is_degenerate()) {
            throw "Input facet components contains degenerated triangles";
        }
    }
    Tree tree(triangles.begin(), triangles.end());
    tree.accelerate_distance_queries();

    enum ElementType { VERTEX, EDGE, FACE };
    auto determine_element_type = [&](
            size_t fid, const Point_3& p, size_t& element_index) -> ElementType{
        const Eigen::Vector3i f = F.row(I(fid, 0));
        const Point_3 p0(V(f[0], 0), V(f[0], 1), V(f[0], 2));
        const Point_3 p1(V(f[1], 0), V(f[1], 1), V(f[1], 2));
        const Point_3 p2(V(f[2], 0), V(f[2], 1), V(f[2], 2));

        if (p == p0) { element_index = 0; return VERTEX; }
        if (p == p1) { element_index = 1; return VERTEX; }
        if (p == p2) { element_index = 2; return VERTEX; }
        if (CGAL::collinear(p0, p1, p)) { element_index = 2; return EDGE; }
        if (CGAL::collinear(p1, p2, p)) { element_index = 0; return EDGE; }
        if (CGAL::collinear(p2, p0, p)) { element_index = 1; return EDGE; }

        element_index = 0;
        return FACE;
    };

    const size_t num_queries = P.rows();
    inside.resize(num_queries, 1);
    for (size_t i=0; i<num_queries; i++) {
        const Point_3 query(P(i,0), P(i,1), P(i,2));
        auto projection = tree.closest_point_and_primitive(query);
        auto closest_point = projection.first;
        size_t fid = projection.second - triangles.begin();

        size_t element_index;
        switch (determine_element_type(fid, closest_point, element_index)) {
            case VERTEX:
                {
                    const size_t s = F(I(fid, 0), element_index);
                    inside(i,0) = determine_point_vertex_orientation(
                            V, F, I, query, s);
                }
                break;
            case EDGE:
                {
                    const size_t s = F(I(fid, 0), (element_index+1)%3);
                    const size_t d = F(I(fid, 0), (element_index+2)%3);
                    inside(i,0) = determine_point_edge_orientation(
                            V, F, I, query, s, d);
                }
                break;
            case FACE:
                inside(i,0) = determine_point_face_orientation(V, F, I, query, fid);
                break;
            default:
                throw "Unknown closest element type!";
        }
    }
}

template<typename DerivedV, typename DerivedF, typename DerivedP,
    typename DerivedB>
IGL_INLINE void igl::copyleft::cgal::points_inside_component(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedP>& P,
        Eigen::PlainObjectBase<DerivedB>& inside) {
    Eigen::VectorXi I = igl::LinSpaced<Eigen::VectorXi>(F.rows(), 0, F.rows()-1);
    igl::copyleft::cgal::points_inside_component(V, F, I, P, inside);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::cgal::points_inside_component<Eigen::Matrix<double, -1,   -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>,   Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3>,   Eigen::Array<bool, -1, 1, 0, -1, 1>   >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&,   Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&,   Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3,   0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Array<bool, -1, 1, 0, -1, 1>   >&);
template void igl::copyleft::cgal::points_inside_component< Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<   int, -1, -1, 0, -1, -1>, Eigen::Matrix<   int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<   int, -1, -1, 0, -1, -1> > ( Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<   int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<   int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<   int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::cgal::points_inside_component< Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<   int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<   int, -1, -1, 0, -1, -1> > ( Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix< int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<   int, -1, -1, 0, -1, -1> >&);
template void igl::copyleft::cgal::points_inside_component<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::points_inside_component<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::points_inside_component<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::copyleft::cgal::points_inside_component<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
