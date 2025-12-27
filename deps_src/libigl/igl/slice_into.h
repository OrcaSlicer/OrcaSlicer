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
  // Act like the matlab Y(row_indices,col_indices) = X
  // 
  // Inputs:
  //   X  xm by xn rhs matrix
  //   R  list of row indices
  //   C  list of column indices
  //   Y  ym by yn lhs matrix
  // Output:
  //   Y  ym by yn lhs matrix, same as input but Y(R,C) = X
  template <typename T>
  IGL_INLINE void slice_into(
    const Eigen::SparseMatrix<T>& X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & C,
    Eigen::SparseMatrix<T>& Y);

  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void slice_into(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & C,
    Eigen::PlainObjectBase<DerivedY> & Y);
  // Wrapper to only slice in one direction
  //
  // Inputs:
  //   dim  dimension to slice in 1 or 2, dim=1 --> X(R,:), dim=2 --> X(:,R)
  //
  // Note: For now this is just a cheap wrapper.
  template <typename MatX, typename MatY>
  IGL_INLINE void slice_into(
    const MatX & X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    const int dim,
    MatY& Y);

  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void slice_into(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    Eigen::PlainObjectBase<DerivedY> & Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "slice_into.cpp"
#endif

#endif
