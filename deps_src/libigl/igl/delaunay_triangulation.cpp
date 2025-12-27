// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Qingnan Zhou <qnzhou@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "delaunay_triangulation.h"
#include "flip_edge.h"
#include "lexicographic_triangulation.h"
#include "unique_edge_map.h"

#include <vector>
#include <sstream>

template<
  typename DerivedV,
  typename Orient2D,
  typename InCircle,
  typename DerivedF>
IGL_INLINE void igl::delaunay_triangulation(
    const Eigen::PlainObjectBase<DerivedV>& V,
    Orient2D orient2D,
    InCircle incircle,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  assert(V.cols() == 2);
  typedef typename DerivedF::Scalar Index;
  typedef typename DerivedV::Scalar Scalar;
  igl::lexicographic_triangulation(V, orient2D, F);
  const size_t num_faces = F.rows();
  if (num_faces == 0) {
    // Input points are degenerate.  No faces will be generated.
    return;
  }
  assert(F.cols() == 3);

  Eigen::MatrixXi E;
  Eigen::MatrixXi uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<Index> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);

  auto is_delaunay = [&V,&F,&uE2E,num_faces,&incircle](size_t uei) {
    auto& half_edges = uE2E[uei];
    if (half_edges.size() != 2) {
      throw "Cannot flip non-manifold or boundary edge";
    }

    const size_t f1 = half_edges[0] % num_faces;
    const size_t f2 = half_edges[1] % num_faces;
    const size_t c1 = half_edges[0] / num_faces;
    const size_t c2 = half_edges[1] / num_faces;
    assert(c1 < 3);
    assert(c2 < 3);
    assert(f1 != f2);
    const size_t v1 = F(f1, (c1+1)%3);
    const size_t v2 = F(f1, (c1+2)%3);
    const size_t v4 = F(f1, c1);
    const size_t v3 = F(f2, c2);
    const Scalar p1[] = {V(v1, 0), V(v1, 1)};
    const Scalar p2[] = {V(v2, 0), V(v2, 1)};
    const Scalar p3[] = {V(v3, 0), V(v3, 1)};
    const Scalar p4[] = {V(v4, 0), V(v4, 1)};
    auto orientation = incircle(p1, p2, p4, p3);
    return orientation <= 0;
  };

  bool all_delaunay = false;
  while(!all_delaunay) {
    all_delaunay = true;
    for (size_t i=0; i<uE2E.size(); i++) {
      if (uE2E[i].size() == 2) {
        if (!is_delaunay(i)) {
          all_delaunay = false;
          flip_edge(F, E, uE, EMAP, uE2E, i);
        }
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::delaunay_triangulation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, short (*)(double const*, double const*, double const*), short (*)(double const*, double const*, double const*, double const*), Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, short (*)(double const*, double const*, double const*), short (*)(double const*, double const*, double const*, double const*), Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif