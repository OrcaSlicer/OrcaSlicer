// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_WINDING_NUMBER_H
#define IGL_WINDING_NUMBER_H
#include "igl_inline.h"
#include <Eigen/Core>

// Minimum number of iterms per openmp thread
#ifndef IGL_WINDING_NUMBER_OMP_MIN_VALUE
#  define IGL_WINDING_NUMBER_OMP_MIN_VALUE 1000
#endif
namespace igl
{
  // WINDING_NUMBER Compute the sum of solid angles of a triangle/tetrahedron
  // described by points (vectors) V
  //
  // Templates:
  //   dim  dimension of input
  // Inputs:
  //  V  n by 3 list of vertex positions
  //  F  #F by 3 list of triangle indices, minimum index is 0
  //  O  no by 3 list of origin positions
  // Outputs:
  //  S  no by 1 list of winding numbers
  //
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedO,
    typename DerivedW>
  IGL_INLINE void winding_number(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedO> & O,
    Eigen::PlainObjectBase<DerivedW> & W);
  // Compute winding number of a single point
  //
  // Inputs:
  //  V  n by dim list of vertex positions
  //  F  #F by dim list of triangle indices, minimum index is 0
  //  p  single origin position
  // Outputs:
  //  w  winding number of this point
  //
  template <
    typename DerivedV,
    typename DerivedF,
    typename Derivedp>
  IGL_INLINE typename DerivedV::Scalar winding_number(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<Derivedp> & p);
}

#ifndef IGL_STATIC_LIBRARY
#  include "winding_number.cpp"
#endif

#endif
