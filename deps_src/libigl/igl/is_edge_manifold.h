// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_EDGE_MANIFOLD_H
#define IGL_IS_EDGE_MANIFOLD_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl 
{
  // check if the mesh is edge-manifold
  //
  // Inputs:
  //   V  #V by dim list of mesh vertex positions **unneeded**
  //   F  #F by 3 list of triangle indices
  // Returns whether mesh is edge manifold.
  //
  // Known Bugs:
  //  Does not check for non-manifold vertices
  //
  // See also: is_vertex_manifold
  template <
    typename DerivedF, 
    typename DerivedBF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedBE>
  IGL_INLINE bool is_edge_manifold(
    const Eigen::MatrixBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedBF>& BF,
    Eigen::PlainObjectBase<DerivedE>& E,
    Eigen::PlainObjectBase<DerivedEMAP>& EMAP,
    Eigen::PlainObjectBase<DerivedBE>& BE);
  template <typename DerivedF>
  IGL_INLINE bool is_edge_manifold(
    const Eigen::MatrixBase<DerivedF>& F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_edge_manifold.cpp"
#endif

#endif
