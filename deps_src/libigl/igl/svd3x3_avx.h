// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SVD3X3_AVX_H
#define IGL_SVD3X3_AVX_H
#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl
{
  // Super fast 3x3 SVD according to
  // http://pages.cs.wisc.edu/~sifakis/project_pages/svd.html This is AVX
  // version of svd3x3 (see svd3x3.h) which works on 8 matrices at a time These
  // eight matrices are simply stacked in columns, the rest is the same as for
  // svd3x3
  //
  // Inputs:
  //   A  12 by 3 stack of 3x3 matrices
  // Outputs:
  //   U  12x3 left singular vectors stacked
  //   S  12x1 singular values stacked
  //   V  12x3 right singular vectors stacked
  //
  // Known bugs: this will not work correctly for double precision.
  template<typename T>
  IGL_INLINE void svd3x3_avx(
    const Eigen::Matrix<T, 3*8, 3>& A, 
    Eigen::Matrix<T, 3*8, 3> &U, 
    Eigen::Matrix<T, 3*8, 1> &S, 
    Eigen::Matrix<T, 3*8, 3>&V);
}
#ifndef IGL_STATIC_LIBRARY
#  include "svd3x3_avx.cpp"
#endif
#endif

