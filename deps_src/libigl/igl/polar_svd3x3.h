// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POLAR_SVD3X3_H
#define IGL_POLAR_SVD3X3_H
#include <Eigen/Core>
#include "igl_inline.h"
namespace igl
{
  /// Computes the closest rotation to input matrix A using specialized 3x3 SVD
  /// singular value decomposition (WunderSVD3x3)
  ///
  /// @param[in] A  3 by 3 matrix to be decomposed
  /// @param[out] R  3 by 3 closest element in SO(3) (closeness in terms of Frobenius
  ///   metric)
  ///
  /// This means that det(R) = 1. Technically it's not polar decomposition
  /// which guarantees positive semidefinite stretch factor (at the cost of
  /// having det(R) = -1). "• The orthogonal factors U and V will be true
  /// rotation matrices..." [McAdams, Selle, Tamstorf, Teran, Sefakis 2011]
  ///
  template<typename Mat>
  IGL_INLINE void polar_svd3x3(const Mat& A, Mat& R);
  #ifdef __SSE__
  /// \overload
  template<typename T>
  IGL_INLINE void polar_svd3x3_sse(const Eigen::Matrix<T, 3*4, 3>& A, Eigen::Matrix<T, 3*4, 3> &R);
  #endif
  #ifdef __AVX__
  /// \overload
  template<typename T>
  IGL_INLINE void polar_svd3x3_avx(const Eigen::Matrix<T, 3*8, 3>& A, Eigen::Matrix<T, 3*8, 3> &R);
  #endif
}
#ifndef IGL_STATIC_LIBRARY
#  include "polar_svd3x3.cpp"
#endif
#endif

