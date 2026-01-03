// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "outer_facet.h"
#include "outer_edge.h"
#include "order_facets_around_edge.h"
#include "../../PlainMatrix.h"
#include <algorithm>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedI,
    typename IndexType
    >
IGL_INLINE void igl::copyleft::cgal::outer_facet(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const Eigen::MatrixBase<DerivedI> & I,
        IndexType & f,
        bool & flipped) {

    // Algorithm:
    //
    //    1. Find an outer edge (s, d).
    //
    //    2. Order adjacent facets around this edge. Because the edge is an
    //    outer edge, there exists a plane passing through it such that all its
    //    adjacent facets lie on the same side. The implementation of
    //    order_facets_around_edge() will find a natural start facet such that
    //    The first and last facets according to this order are on the outside.
    //
    //    3. Because the vertex s is an outer vertex by construction (see
    //    implemnetation of outer_edge()). The first adjacent facet is facing
    //    outside (i.e. flipped=false) if it has positive X normal component.
    //    If it has zero normal component, it is facing outside if it contains
    //    directed edge (s, d).

    //typedef typename DerivedV::Scalar Scalar;
    typedef typename DerivedV::Index Index;

    Index s,d;
    Eigen::Matrix<Index,Eigen::Dynamic,1> incident_faces;
    outer_edge(V, F, I, s, d, incident_faces);
    assert(incident_faces.size() > 0);

    auto convert_to_signed_index = [&](size_t fid) -> int{
        if ((F(fid, 0) == s && F(fid, 1) == d) ||
            (F(fid, 1) == s && F(fid, 2) == d) ||
            (F(fid, 2) == s && F(fid, 0) == d) ) {
            return int(fid+1) * -1;
        } else {
            return int(fid+1);
        }
    };

    auto signed_index_to_index = [&](int signed_id) -> size_t {
        return size_t(abs(signed_id) - 1);
    };

    std::vector<int> adj_faces(incident_faces.size());
    std::transform(incident_faces.data(),
            incident_faces.data() + incident_faces.size(),
            adj_faces.begin(),
            convert_to_signed_index);

    PlainMatrix<DerivedV,1> pivot_point = V.row(s);
    pivot_point(0, 0) += 1.0;

    Eigen::VectorXi order;
    order_facets_around_edge(V, F, s, d, adj_faces, pivot_point, order);

    f = signed_index_to_index(adj_faces[order[0]]);
    flipped = adj_faces[order[0]] > 0;
}



template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedI,
    typename IndexType
    >
IGL_INLINE void igl::copyleft::cgal::outer_facet(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F,
        const Eigen::MatrixBase<DerivedN> & N,
        const Eigen::MatrixBase<DerivedI> & I,
        IndexType & f,
        bool & flipped) {
    // Algorithm:
    //    Find an outer edge.
    //    Find the incident facet with the largest absolute X normal component.
    //    If there is a tie, keep the one with positive X component.
    //    If there is still a tie, pick the face with the larger signed index
    //    (flipped face has negative index).
    typedef typename DerivedV::Scalar Scalar;
    typedef typename DerivedV::Index Index;
    const size_t INVALID = std::numeric_limits<size_t>::max();

    Index v1,v2;
    Eigen::Matrix<Index,Eigen::Dynamic,1> incident_faces;
    outer_edge(V, F, I, v1, v2, incident_faces);
    assert(incident_faces.size() > 0);

    auto generic_fabs = [&](const Scalar& val) -> const Scalar {
        if (val >= 0) return val;
        else return -val;
    };

    Scalar max_nx = 0;
    size_t outer_fid = INVALID;
    const size_t num_incident_faces = incident_faces.size();
    for (size_t i=0; i<num_incident_faces; i++)
    {
        const auto& fid = incident_faces(i);
        const Scalar nx = N(fid, 0);
        if (outer_fid == INVALID) {
            max_nx = nx;
            outer_fid = fid;
        } else {
            if (generic_fabs(nx) > generic_fabs(max_nx)) {
                max_nx = nx;
                outer_fid = fid;
            } else if (nx == -max_nx && nx > 0) {
                max_nx = nx;
                outer_fid = fid;
            } else if (nx == max_nx) {
                if ((max_nx >= 0 && outer_fid < fid) ||
                    (max_nx <  0 && outer_fid > fid)) {
                    max_nx = nx;
                    outer_fid = fid;
                }
            }
        }
    }

    assert(outer_fid != INVALID);
    f = outer_fid;
    flipped = max_nx < 0;
}

#ifdef IGL_STATIC_LIBRARY
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
// Explicit template instantiation
// generated by autoexplicit.sh
#include <cstdint>
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, std::ptrdiff_t>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, std::ptrdiff_t&, bool&);
// generated by autoexplicit.sh
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int&, bool&);
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Index>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Index &, bool&);
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int&, bool&);
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Index>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::Index&, bool&);
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, int>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int&, bool&);
template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int&, bool&);
//template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> > const&, int&, bool&);
//template void igl::copyleft::cgal::outer_facet<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, int>(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> > const&, int&, bool&);
#ifdef WIN32
#endif
#endif
