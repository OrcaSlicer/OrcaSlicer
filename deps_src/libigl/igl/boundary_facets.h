// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BOUNDARY_FACETS_H
#define IGL_BOUNDARY_FACETS_H
#include "igl_inline.h"

#include <Eigen/Dense>

#include <vector>

namespace igl
{
  // BOUNDARY_FACETS Determine boundary faces (edges) of tetrahedra (triangles)
  // stored in T (analogous to qptoolbox's `outline` and `boundary_faces`).
  //
  // Templates:
  //   IntegerT  integer-value: e.g. int
  //   IntegerF  integer-value: e.g. int
  // Input:
  //  T  tetrahedron (triangle) index list, m by 4 (3), where m is the number of tetrahedra
  // Output:
  //  F  list of boundary faces, n by 3 (2), where n is the number of boundary faces
  //
  //
  template <typename IntegerT, typename IntegerF>
  IGL_INLINE void boundary_facets(
    const std::vector<std::vector<IntegerT> > & T,
    std::vector<std::vector<IntegerF> > & F);

  // Templates:
  //   DerivedT  integer-value: i.e. from MatrixXi
  //   DerivedF  integer-value: i.e. from MatrixXi
  template <typename DerivedT, typename DerivedF>
  IGL_INLINE void boundary_facets(
    const Eigen::PlainObjectBase<DerivedT>& T,
    Eigen::PlainObjectBase<DerivedF>& F);
  // Same as above but returns F
  template <typename DerivedT, typename Ret>
  Ret boundary_facets(
    const Eigen::PlainObjectBase<DerivedT>& T);
}

#ifndef IGL_STATIC_LIBRARY
#  include "boundary_facets.cpp"
#endif

#endif
