// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_DELAUNAY_H
#define IGL_IS_DELAUNAY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// IDetermine if each edge in the mesh (V,F) is Delaunay.
  ///
  /// @param[in] V  #V by dim list of vertex positions
  /// @param[in] F  #F by 3 list of triangles indices
  /// @param[out] D  #F by 3 list of bools revealing whether edges corresponding 23 31 12
  ///     are locally Delaunay. Boundary edges are by definition Delaunay.
  ///     Non-Manifold edges are by definition not Delaunay.
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedD>
  IGL_INLINE void is_delaunay(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedD> & D);
  /// Determine whether a single edge is Delaunay using a provided (extrinsic) incirle
  /// test.
  ///
  /// @param[in] V  #V by dim list of vertex positions
  /// @param[in] F  #F by 3 list of triangles indices
  /// @param[in] uE2E  #uE list of lists of indices into E of coexisting edges (see
  ///     unique_edge_map)
  /// @param[in] incircle  A functor such that incircle(pa, pb, pc, pd) returns
  ///               1 if pd is on the positive size of circumcirle of (pa,pb,pc)
  ///              -1 if pd is on the positive size of circumcirle of (pa,pb,pc)
  ///               0 if pd is cocircular with pa, pb, pc.
  ///               (see delaunay_triangulation)
  /// @param[in] uei  index into uE2E of edge to check
  /// @return true iff edge is Delaunay
  template <
    typename DerivedV,
    typename DerivedF,
    typename uE2EType,
    typename InCircle,
    typename ueiType>
  IGL_INLINE bool is_delaunay(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const std::vector<std::vector<uE2EType> > & uE2E,
    const InCircle incircle,
    const ueiType uei);

}
#ifndef IGL_STATIC_LIBRARY
#include "is_delaunay.cpp"
#endif
#endif
