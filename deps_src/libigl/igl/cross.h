// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CROSS_H
#define IGL_CROSS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Computes out = cross(a,b)
  // Inputs:
  //   a  left 3d vector
  //   b  right 3d vector
  // Outputs:
  //   out  result 3d vector
  IGL_INLINE void cross( const double *a, const double *b, double *out);
  // Computes C = cross(A,B,2);
  //
  // Inputs:
  //   A  #A by 3 list of row-vectors
  //   B  #A by 3 list of row-vectors
  // Outputs:
  //   C  #A by 3 list of row-vectors
  template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC>
  IGL_INLINE void cross(
    const Eigen::PlainObjectBase<DerivedA> & A,
    const Eigen::PlainObjectBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cross.cpp"
#endif

#endif
