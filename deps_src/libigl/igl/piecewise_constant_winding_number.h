// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PIECEWISE_CONSTANT_WINDING_NUMBER_H
#define IGL_PIECEWISE_CONSTANT_WINDING_NUMBER_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  // PIECEWISE_CONSTANT_WINDING_NUMBER Determine if a given mesh induces a
  // piecewise constant winding number field: Is this mesh valid input to solid
  // set operations.  **Assumes** that `(V,F)` contains no self-intersections
  // (including degeneracies and co-incidences).  If there are co-planar and
  // co-incident vertex placements, a mesh could _fail_ this combinatorial test
  // but still induce a piecewise-constant winding number _geometrically_. For
  // example, consider a hemisphere with boundary and then pinch the boundary
  // "shut" along a line segment. The **_bullet-proof_** check is to first
  // resolve all self-intersections in `(V,F) -> (SV,SF)` (i.e. what the
  // `igl::copyleft::cgal::piecewise_constant_winding_number` overload does).
  //
  // Inputs:
  //   F  #F by 3 list of triangle indices into some (abstract) list of
  //     vertices V
  //   uE  #uE by 2 list of unique edges indices into V
  //   uE2E  #uE list of lists of indices into directed edges (#F * 3)
  // Returns true if the mesh _combinatorially_ induces a piecewise constant
  // winding number field.
  //
  template <
    typename DerivedF,
    typename DeriveduE,
    typename uE2EType>
  IGL_INLINE bool piecewise_constant_winding_number(
    const Eigen::MatrixBase<DerivedF>& F,
    const Eigen::MatrixBase<DeriveduE>& uE,
    const std::vector<std::vector<uE2EType> >& uE2E);
  template <typename DerivedF>
  IGL_INLINE bool piecewise_constant_winding_number(
    const Eigen::MatrixBase<DerivedF>& F);
}
#ifndef IGL_STATIC_LIBRARY
#  include "piecewise_constant_winding_number.cpp"
#endif
#endif
