// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EXTERIOR_EDGES_H
#define IGL_EXTERIOR_EDGES_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  /// Determines boundary "edges" and also edges with an odd number of
  /// occurrences where seeing edge (i,j) counts as +1 and seeing the opposite
  /// edge (j,i) counts as -1
  ///
  /// @param[in] F  #F by simplex_size list of "faces"
  /// @param[out] E  #E by simplex_size-1  list of exterior edges
  ///
  template <typename DerivedF, typename DerivedE>
  IGL_INLINE void exterior_edges(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E);
}
#ifndef IGL_STATIC_LIBRARY
#  include "exterior_edges.cpp"
#endif

#endif
