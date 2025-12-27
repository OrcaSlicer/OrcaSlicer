// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ALL_EDGES_H
#define IGL_ALL_EDGES_H
#include "igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  // Deprecated: call oriented_facets instead.
  //
  // ALL_EDGES Determines all "directed edges" of a given set of simplices. For
  // a manifold mesh, this computes all of the half-edges
  //
  // Inputs:
  //   F  #F by simplex_size list of "faces"
  // Outputs:
  //   E  #E by simplex_size-1  list of edges
  //
  // Note: this is not the same as igl::edges because this includes every
  // directed edge including repeats (meaning interior edges on a surface will
  // show up once for each direction and non-manifold edges may appear more than
  // once for each direction).
  template <typename DerivedF, typename DerivedE>
  IGL_INLINE void all_edges(
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedE> & E);
}

#ifndef IGL_STATIC_LIBRARY
#  include "all_edges.cpp"
#endif

#endif
