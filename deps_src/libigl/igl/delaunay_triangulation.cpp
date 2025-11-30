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
#include "is_delaunay.h"

#include <vector>
#include <sstream>

template<
  typename DerivedV,
  typename Orient2D,
  typename InCircle,
  typename DerivedF>
IGL_INLINE void igl::delaunay_triangulation(
    const Eigen::MatrixBase<DerivedV>& V,
    Orient2D orient2D,
    InCircle incircle,
    Eigen::PlainObjectBase<DerivedF>& F)
{
  assert(V.cols() == 2);
  typedef typename DerivedF::Scalar Index;
  igl::lexicographic_triangulation(V, orient2D, F);
  const size_t num_faces = F.rows();
  if (num_faces == 0) {
    // Input points are degenerate.  No faces will be generated.
    return;
  }
  assert(F.cols() == 3);

  typedef Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> MatrixX2I;
  MatrixX2I E,uE;
  Eigen::VectorXi EMAP;
  std::vector<std::vector<Index> > uE2E;
  igl::unique_edge_map(F, E, uE, EMAP, uE2E);

  bool all_delaunay = false;
  while(!all_delaunay) {
    all_delaunay = true;
    for (size_t i=0; i<uE2E.size(); i++) {
      if (uE2E[i].size() == 2) {
        if (!is_delaunay(V,F,uE2E,incircle,i)) {
          all_delaunay = false;
          flip_edge(F, E, uE, EMAP, uE2E, i);
        }
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::delaunay_triangulation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, short (*)(double const*, double const*, double const*), short (*)(double const*, double const*, double const*, double const*), Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, short (*)(double const*, double const*, double const*), short (*)(double const*, double const*, double const*, double const*), Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
