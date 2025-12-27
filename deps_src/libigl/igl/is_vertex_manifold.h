// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_VERTEX_MANIFOLD_H
#define IGL_IS_VERTEX_MANIFOLD_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl 
{
  // Check if a mesh is vertex-manifold. This only checks whether the faces
  // incident on each vertex form exactly one connected component. Vertices
  // incident on non-manifold edges are not consider non-manifold by this
  // function (see is_edge_manifold.h). Unreferenced verties are considered
  // non-manifold (zero components).
  //
  // Inputs:
  //   F  #F by 3 list of triangle indices
  // Outputs:
  //   B  #V list indicate whether each vertex is locally manifold.
  // Returns whether mesh is vertex manifold.
  //
  // See also: is_edge_manifold
  template <typename DerivedF,typename DerivedB>
  IGL_INLINE bool is_vertex_manifold(
    const Eigen::PlainObjectBase<DerivedF>& F,
    Eigen::PlainObjectBase<DerivedB>& B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_vertex_manifold.cpp"
#endif

#endif
