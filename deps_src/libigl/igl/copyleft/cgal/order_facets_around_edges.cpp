// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "order_facets_around_edges.h"
#include "order_facets_around_edge.h"
#include "../../sort_angles.h"
#include <Eigen/Geometry>
#include <type_traits>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DeriveduE,
    typename uE2EType,
    typename uE2oEType,
    typename uE2CType >
IGL_INLINE
typename std::enable_if<!std::is_same<typename DerivedV::Scalar,
typename CGAL::Exact_predicates_exact_constructions_kernel::FT>::value, void>::type
igl::copyleft::cgal::order_facets_around_edges(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedN>& N,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C ) {

    typedef Eigen::Matrix<typename DerivedN::Scalar, 3, 1> Vector3F;
    const typename DerivedV::Scalar EPS = 1e-12;
    const size_t num_faces = F.rows();
    const size_t num_undirected_edges = uE.rows();

    auto edge_index_to_face_index = [&](size_t ei) { return ei % num_faces; };
    auto edge_index_to_corner_index = [&](size_t ei) { return ei / num_faces; };

    uE2oE.resize(num_undirected_edges);
    uE2C.resize(num_undirected_edges);

    for(size_t ui = 0;ui<num_undirected_edges;ui++)
    {
        const auto& adj_edges = uE2E[ui];
        const size_t edge_valance = adj_edges.size();
        assert(edge_valance > 0);

        const auto ref_edge = adj_edges[0];
        const auto ref_face = edge_index_to_face_index(ref_edge);
        Vector3F ref_normal = N.row(ref_face);

        const auto ref_corner_o = edge_index_to_corner_index(ref_edge);
        const auto ref_corner_s = (ref_corner_o+1)%3;
        const auto ref_corner_d = (ref_corner_o+2)%3;

        const typename DerivedF::Scalar o = F(ref_face, ref_corner_o);
        const typename DerivedF::Scalar s = F(ref_face, ref_corner_s);
        const typename DerivedF::Scalar d = F(ref_face, ref_corner_d);

        Vector3F edge = V.row(d) - V.row(s);
        auto edge_len = edge.norm();
        bool degenerated = edge_len < EPS;
        if (degenerated) {
            if (edge_valance <= 2) {
                // There is only one way to order 2 or less faces.
                edge.setZero();
            } else {
                edge.setZero();
                Eigen::Matrix<typename DerivedN::Scalar, Eigen::Dynamic, 3>
                    normals(edge_valance, 3);
                for (size_t fei=0; fei<edge_valance; fei++) {
                    const auto fe = adj_edges[fei];
                    const auto f = edge_index_to_face_index(fe);
                    normals.row(fei) = N.row(f);
                }
                for (size_t i=0; i<edge_valance; i++) {
                    size_t j = (i+1) % edge_valance;
                    Vector3F ni = normals.row(i);
                    Vector3F nj = normals.row(j);
                    edge = ni.cross(nj);
                    edge_len = edge.norm();
                    if (edge_len >= EPS) {
                        edge.normalize();
                        break;
                    }
                }

                // Ensure edge direction are consistent with reference face.
                Vector3F in_face_vec = V.row(o) - V.row(s);
                if (edge.cross(in_face_vec).dot(ref_normal) < 0) {
                    edge *= -1;
                }

                if (edge.norm() < EPS) {
                    std::cerr << "=====================================" << std::endl;
                    std::cerr << "  ui: " << ui << std::endl;
                    std::cerr << "edge: " << ref_edge << std::endl;
                    std::cerr << "face: " << ref_face << std::endl;
                    std::cerr << "  vs: " << V.row(s) << std::endl;
                    std::cerr << "  vd: " << V.row(d) << std::endl;
                    std::cerr << "adj face normals: " << std::endl;
                    std::cerr << normals << std::endl;
                    std::cerr << "Very degenerated case detected:" << std::endl;
                    std::cerr << "Near zero edge surrounded by "
                        << edge_valance << " neearly colinear faces" <<
                        std::endl;
                    std::cerr << "=====================================" << std::endl;
                }
            }
        } else {
            edge.normalize();
        }

        Eigen::MatrixXd angle_data(edge_valance, 3);
        std::vector<bool> cons(edge_valance);

        for (size_t fei=0; fei<edge_valance; fei++) {
            const auto fe = adj_edges[fei];
            const auto f = edge_index_to_face_index(fe);
            const auto c = edge_index_to_corner_index(fe);
            cons[fei] = (d == F(f, (c+1)%3));
            assert( cons[fei] ||  (d == F(f,(c+2)%3)));
            assert(!cons[fei] || (s == F(f,(c+2)%3)));
            assert(!cons[fei] || (d == F(f,(c+1)%3)));
            Vector3F n = N.row(f);
            angle_data(fei, 0) = ref_normal.cross(n).dot(edge);
            angle_data(fei, 1) = ref_normal.dot(n);
            if (cons[fei]) {
                angle_data(fei, 0) *= -1;
                angle_data(fei, 1) *= -1;
            }
            angle_data(fei, 0) *= -1; // Sort clockwise.
            angle_data(fei, 2) = (cons[fei]?1.:-1.)*(f+1);
        }

        Eigen::VectorXi order;
        igl::sort_angles(angle_data, order);

        auto& ordered_edges = uE2oE[ui];
        auto& consistency = uE2C[ui];

        ordered_edges.resize(edge_valance);
        consistency.resize(edge_valance);
        for (size_t fei=0; fei<edge_valance; fei++) {
            ordered_edges[fei] = adj_edges[order[fei]];
            consistency[fei] = cons[order[fei]];
        }
    }
}

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DeriveduE,
    typename uE2EType,
    typename uE2oEType,
    typename uE2CType >
IGL_INLINE 
typename std::enable_if<std::is_same<typename DerivedV::Scalar,
typename CGAL::Exact_predicates_exact_constructions_kernel::FT>::value, void>::type
igl::copyleft::cgal::order_facets_around_edges(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DerivedN>& N,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C ) {

    typedef Eigen::Matrix<typename DerivedN::Scalar, 3, 1> Vector3F;
    typedef Eigen::Matrix<typename DerivedV::Scalar, 3, 1> Vector3E;
    const typename DerivedV::Scalar EPS = 1e-12;
    const size_t num_faces = F.rows();
    const size_t num_undirected_edges = uE.rows();

    auto edge_index_to_face_index = [&](size_t ei) { return ei % num_faces; };
    auto edge_index_to_corner_index = [&](size_t ei) { return ei / num_faces; };

    uE2oE.resize(num_undirected_edges);
    uE2C.resize(num_undirected_edges);

    for(size_t ui = 0;ui<num_undirected_edges;ui++)
    {
        const auto& adj_edges = uE2E[ui];
        const size_t edge_valance = adj_edges.size();
        assert(edge_valance > 0);

        const auto ref_edge = adj_edges[0];
        const auto ref_face = edge_index_to_face_index(ref_edge);
        Vector3F ref_normal = N.row(ref_face);

        const auto ref_corner_o = edge_index_to_corner_index(ref_edge);
        const auto ref_corner_s = (ref_corner_o+1)%3;
        const auto ref_corner_d = (ref_corner_o+2)%3;

        const typename DerivedF::Scalar o = F(ref_face, ref_corner_o);
        const typename DerivedF::Scalar s = F(ref_face, ref_corner_s);
        const typename DerivedF::Scalar d = F(ref_face, ref_corner_d);

        Vector3E exact_edge = V.row(d) - V.row(s);
        exact_edge.array() /= exact_edge.squaredNorm();
        Vector3F edge(
                CGAL::to_double(exact_edge[0]),
                CGAL::to_double(exact_edge[1]),
                CGAL::to_double(exact_edge[2]));
        edge.normalize();

        Eigen::MatrixXd angle_data(edge_valance, 3);
        std::vector<bool> cons(edge_valance);

        for (size_t fei=0; fei<edge_valance; fei++) {
            const auto fe = adj_edges[fei];
            const auto f = edge_index_to_face_index(fe);
            const auto c = edge_index_to_corner_index(fe);
            cons[fei] = (d == F(f, (c+1)%3));
            assert( cons[fei] ||  (d == F(f,(c+2)%3)));
            assert(!cons[fei] || (s == F(f,(c+2)%3)));
            assert(!cons[fei] || (d == F(f,(c+1)%3)));
            Vector3F n = N.row(f);
            angle_data(fei, 0) = ref_normal.cross(n).dot(edge);
            angle_data(fei, 1) = ref_normal.dot(n);
            if (cons[fei]) {
                angle_data(fei, 0) *= -1;
                angle_data(fei, 1) *= -1;
            }
            angle_data(fei, 0) *= -1; // Sort clockwise.
            angle_data(fei, 2) = (cons[fei]?1.:-1.)*(f+1);
        }

        Eigen::VectorXi order;
        igl::sort_angles(angle_data, order);

        auto& ordered_edges = uE2oE[ui];
        auto& consistency = uE2C[ui];

        ordered_edges.resize(edge_valance);
        consistency.resize(edge_valance);
        for (size_t fei=0; fei<edge_valance; fei++) {
            ordered_edges[fei] = adj_edges[order[fei]];
            consistency[fei] = cons[order[fei]];
        }
    }
}

template<
    typename DerivedV,
    typename DerivedF,
    typename DeriveduE,
    typename uE2EType,
    typename uE2oEType,
    typename uE2CType >
IGL_INLINE void igl::copyleft::cgal::order_facets_around_edges(
        const Eigen::PlainObjectBase<DerivedV>& V,
        const Eigen::PlainObjectBase<DerivedF>& F,
        const Eigen::PlainObjectBase<DeriveduE>& uE,
        const std::vector<std::vector<uE2EType> >& uE2E,
        std::vector<std::vector<uE2oEType> >& uE2oE,
        std::vector<std::vector<uE2CType > >& uE2C ) {

    //typedef Eigen::Matrix<typename DerivedV::Scalar, 3, 1> Vector3E;
    const size_t num_faces = F.rows();
    const size_t num_undirected_edges = uE.rows();

    auto edge_index_to_face_index = [&](size_t ei) { return ei % num_faces; };
    auto edge_index_to_corner_index = [&](size_t ei) { return ei / num_faces; };

    uE2oE.resize(num_undirected_edges);
    uE2C.resize(num_undirected_edges);

    for(size_t ui = 0;ui<num_undirected_edges;ui++)
    {
        const auto& adj_edges = uE2E[ui];
        const size_t edge_valance = adj_edges.size();
        assert(edge_valance > 0);

        const auto ref_edge = adj_edges[0];
        const auto ref_face = edge_index_to_face_index(ref_edge);

        const auto ref_corner_o = edge_index_to_corner_index(ref_edge);
        const auto ref_corner_s = (ref_corner_o+1)%3;
        const auto ref_corner_d = (ref_corner_o+2)%3;

        //const typename DerivedF::Scalar o = F(ref_face, ref_corner_o);
        const typename DerivedF::Scalar s = F(ref_face, ref_corner_s);
        const typename DerivedF::Scalar d = F(ref_face, ref_corner_d);

        std::vector<bool> cons(edge_valance);
        std::vector<int> adj_faces(edge_valance);
        for (size_t fei=0; fei<edge_valance; fei++) {
            const auto fe = adj_edges[fei];
            const auto f = edge_index_to_face_index(fe);
            const auto c = edge_index_to_corner_index(fe);
            cons[fei] = (d == F(f, (c+1)%3));
            adj_faces[fei] = (f+1) * (cons[fei] ? 1:-1);

            assert( cons[fei] ||  (d == F(f,(c+2)%3)));
            assert(!cons[fei] || (s == F(f,(c+2)%3)));
            assert(!cons[fei] || (d == F(f,(c+1)%3)));
        }

        Eigen::VectorXi order;
        order_facets_around_edge(V, F, s, d, adj_faces, order);
        assert((size_t)order.size() == edge_valance);

        auto& ordered_edges = uE2oE[ui];
        auto& consistency = uE2C[ui];

        ordered_edges.resize(edge_valance);
        consistency.resize(edge_valance);
        for (size_t fei=0; fei<edge_valance; fei++) {
            ordered_edges[fei] = adj_edges[order[fei]];
            consistency[fei] = cons[order[fei]];
        }
    }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, int, int, bool>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
// generated by autoexplicit.sh
template std::enable_if<!(std::is_same<Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, CGAL::Lazy_exact_nt<CGAL::Gmpq> >::value), void>::type igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, int, int, bool>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
template void igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, long, long, bool>(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
template void igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, long, long, bool>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
template void igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, long, long, bool>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
template void igl::copyleft::cgal::order_facets_around_edges<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 2, 0, -1, 2>, long, long, bool>(Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Lazy_exact_nt<CGAL::Gmpq>, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 2, 0, -1, 2> > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > > const&, std::vector<std::vector<long, std::allocator<long> >, std::allocator<std::vector<long, std::allocator<long> > > >&, std::vector<std::vector<bool, std::allocator<bool> >, std::allocator<std::vector<bool, std::allocator<bool> > > >&);
#endif
