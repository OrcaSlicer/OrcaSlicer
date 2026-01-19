// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SVD3X3_H
#define IGL_SVD3X3_H
#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl
{
  // Super fast 3x3 SVD according to http://pages.cs.wisc.edu/~sifakis/project_pages/svd.html
  // The resulting decomposition is A = U * diag(S[0], S[1], S[2]) * V'
  // BEWARE: this SVD algorithm guarantees that det(U) = det(V) = 1, but this 
  // comes at the cost that 'sigma3' can be negative
  // for computing polar decomposition it's great because all we need to do is U*V'
  // and the result will automatically have positive determinant
  //
  // Inputs:
  //   A  3x3 matrix
  // Outputs:
  //   U  3x3 left singular vectors
  //   S  3x1 singular values
  //   V  3x3 right singular vectors
  //
  // Known bugs: this will not work correctly for double precision.
  template<typename T>
  IGL_INLINE void svd3x3(
    const Eigen::Matrix<T, 3, 3>& A, 
    Eigen::Matrix<T, 3, 3> &U, 
    Eigen::Matrix<T, 3, 1> &S, 
    Eigen::Matrix<T, 3, 3>&V);
}
#ifndef IGL_STATIC_LIBRARY
#  include "svd3x3.cpp"
#endif
#endif
