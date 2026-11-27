// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CAT_H
#define IGL_CAT_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>

namespace igl
{
  /// Perform concatenation of a two _sparse_ matrices along a single dimension
  /// If dim == 1, then C = [A;B]; If dim == 2 then C = [A B].
  /// This is an attempt to act like matlab's cat function.
  /// 
  /// @tparam  Scalar  scalar data type for sparse matrices like double or int
  /// @tparam  Mat  matrix type for all matrices (e.g. MatrixXd, SparseMatrix)
  /// @tparam  MatC  matrix type for output matrix (e.g. MatrixXd) needs to support
  ///     resize
  /// @param[in]  dim  dimension along which to concatenate, 1 or 2
  /// @param[in]  A  first input matrix
  /// @param[in]  B  second input matrix
  /// @param[out]  C  output matrix
  ///   
  template <typename Scalar>
  IGL_INLINE void cat(
      const int dim, 
      const Eigen::SparseMatrix<Scalar> & A, 
      const Eigen::SparseMatrix<Scalar> & B, 
      Eigen::SparseMatrix<Scalar> & C);

  /// Perform concatenation of a two _dense_ matrices along a single dimension
  /// If dim == 1, then C = [A;B]; If dim == 2 then C = [A B].
  ///
  /// @param[in]  dim  dimension along which to concatenate, 1 or 2
  /// @param[in]  A  first input matrix
  /// @param[in]  B  second input matrix
  /// @param[out]  C  output matrix
  ///
  /// \note If you're using Dense matrices you might be better off using the << operator
  template <typename Derived, class MatC>
  IGL_INLINE void cat(
    const int dim,
    const Eigen::MatrixBase<Derived> & A, 
    const Eigen::MatrixBase<Derived> & B,
    MatC & C);
  /// Perform concatenation of a two _dense_ matrices along a single dimension
  /// If dim == 1, then C = [A;B]; If dim == 2 then C = [A B].
  ///
  /// @param[in]  dim  dimension along which to concatenate, 1 or 2
  /// @param[in]  A  first input matrix
  /// @param[in]  B  second input matrix
  /// @return C  output matrix
  ///
  /// \note If you're using Dense matrices you might be better off using the << operator
  template <class Mat>
  IGL_INLINE Mat cat(const int dim, const Mat & A, const Mat & B);
  /// Concatenate a "matrix" of sub-blocks
  /// C = [A0;A1;A2;...;An] where Ai = [A[i][0] A[i][1] ... A[i][m]];
  ///
  /// @param[in]  A  a list of list of matrices (sizes must be compatibile)
  /// @param[out]  C output matrix
  template <class Mat>
  IGL_INLINE void cat(const std::vector<std::vector< Mat > > & A, Mat & C);
  /// Concatenate a std::vector of matrices along the specified dimension
  ///
  /// @param[in] dim  dimension along which to concatenate, 1 or 2
  /// @param[in] A  std::vector of eigen matrices. Must have identical # cols if dim == 1 or rows if dim == 2
  /// @param[out] C  output matrix
  template <typename T, typename DerivedC>
  IGL_INLINE void cat(const int dim, const std::vector<T> & A, Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cat.cpp"
#endif

#endif
