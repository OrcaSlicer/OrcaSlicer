// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REPDIAG_H
#define IGL_REPDIAG_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  /// Repeat a matrix along the diagonal a certain number of times, so
  /// that if A is a m by n matrix and we want to repeat along the diagonal d
  /// times, we get a m*d by n*d matrix B such that:
  /// B( (k*m+1):(k*m+1+m-1), (k*n+1):(k*n+1+n-1)) = A 
  /// for k from 0 to d-1
  ///
  /// @param[in] A  m by n matrix we are repeating along the diagonal.
  /// @param[in] d  number of times to repeat A along the diagonal
  /// @param[out] B  m*d by n*d matrix with A repeated d times along the diagonal,
  ///     will be dense or sparse to match A
  ///
  template <typename T>
  IGL_INLINE void repdiag(
    const Eigen::SparseMatrix<T>& A,
    const int d,
    Eigen::SparseMatrix<T>& B);
  /// \overload
  template <typename T>
  IGL_INLINE void repdiag(
    const Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & A,
    const int d,
    Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & B);
  /// \overload
  template <class Mat>
  IGL_INLINE Mat repdiag(const Mat & A, const int d);
}

#ifndef IGL_STATIC_LIBRARY
#  include "repdiag.cpp"
#endif

#endif
