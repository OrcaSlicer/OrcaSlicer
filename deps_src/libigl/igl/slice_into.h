// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SLICE_INTO_H
#define IGL_SLICE_INTO_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  /// Act like the matlab Y(row_indices,col_indices) = X
  /// 
  /// @param[in] X  xm by xn rhs matrix
  /// @param[in] R  list of row indices
  /// @param[in] C  list of column indices
  /// @param[in] Y  ym by yn lhs matrix
  /// @param[out] Y  ym by yn lhs matrix, same as input but Y(R,C) = X
  ///
  ///
  /// \see slice
  template <typename T, typename DerivedR, typename DerivedC>
  IGL_INLINE void slice_into(
    const Eigen::SparseMatrix<T>& X,
    const Eigen::MatrixBase<DerivedR> & R,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::SparseMatrix<T>& Y);
  /// \overload
  /// \brief Wrapper to only slice in one direction
  ///
  /// @param[int] dim  dimension to slice in 1 or 2, dim=1 --> X(R,:), dim=2 --> X(:,R)
  ///
  /// \note For now this is just a cheap wrapper.
  template <typename MatX, typename MatY, typename DerivedR>
  IGL_INLINE void slice_into(
    const MatX & X,
    const Eigen::MatrixBase<DerivedR> & R,
    const int dim,
    MatY& Y);
  /// \overload
  ///
  /// \deprecated
  /// 
  /// See slice.h for more details
  template <typename DerivedX, typename DerivedY, typename DerivedR, typename DerivedC>
  IGL_INLINE void slice_into(
    const Eigen::MatrixBase<DerivedX> & X,
    const Eigen::MatrixBase<DerivedR> & R,
    const Eigen::MatrixBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedY> & Y);
  /// \overload
  /// \brief Vector version
  template <typename DerivedX, typename DerivedR, typename DerivedY>
  IGL_INLINE void slice_into(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedY>& Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "slice_into.cpp"
#endif

#endif
