// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGES_H
#define IGL_EDGES_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl
{
  // Constructs a list of unique edges represented in a given mesh (V,F)
  // Templates:
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Inputs:
  //   F  #F by 3 list of mesh faces (must be triangles)
  //   or
  //   T  #T x 4  matrix of indices of tet corners
  // Outputs:
  //   E #E by 2 list of edges in no particular order
  //
  // See also: adjacency_matrix
  template <typename DerivedF, typename DerivedE>
  IGL_INLINE void edges(
    const Eigen::MatrixBase<DerivedF> & F, 
    Eigen::PlainObjectBase<DerivedE> & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "edges.cpp"
#endif

#endif
