// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SLICE_H
#define IGL_SLICE_H
#include "igl_inline.h"

#include <Eigen/Sparse>
namespace igl
{
  // Act like the matlab X(row_indices,col_indices) operator, where
  // row_indices, col_indices are non-negative integer indices.
  // 
  // Inputs:
  //   X  m by n matrix
  //   R  list of row indices
  //   C  list of column indices
  // Output:
  //   Y  #R by #C matrix
  //
  // See also: slice_mask
  template <
    typename TX, 
    typename TY>
  IGL_INLINE void slice(
    const Eigen::SparseMatrix<TX>& X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & C,
    Eigen::SparseMatrix<TY>& Y);
  // Wrapper to only slice in one direction
  //
  // Inputs:
  //   dim  dimension to slice in 1 or 2, dim=1 --> X(R,:), dim=2 --> X(:,R)
  //
  // Note: For now this is just a cheap wrapper.
  template <
    typename MatX, 
    typename DerivedR,
    typename MatY>
  IGL_INLINE void slice(
    const MatX& X,
    const Eigen::DenseBase<DerivedR> & R,
    const int dim,
    MatY& Y);
  template <
    typename DerivedX, 
    typename DerivedR, 
    typename DerivedC, 
    typename DerivedY>
  IGL_INLINE void slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::DenseBase<DerivedR> & R,
    const Eigen::DenseBase<DerivedC> & C,
    Eigen::PlainObjectBase<DerivedY> & Y);

  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    Eigen::PlainObjectBase<DerivedY> & Y);
  // VectorXi Y = slice(X,R);
  //
  // This templating is bad because the return type might not have the same
  // size as `DerivedX`. This will probably only work if DerivedX has Dynamic
  // as it's non-trivial sizes or if the number of rows in R happens to equal
  // the number of rows in `DerivedX`.
  template <typename DerivedX>
  IGL_INLINE DerivedX slice(
    const Eigen::DenseBase<DerivedX> & X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R);
  template <typename DerivedX>
  IGL_INLINE DerivedX slice(
    const Eigen::DenseBase<DerivedX>& X,
    const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
    const int dim);

}

#ifndef IGL_STATIC_LIBRARY
#  include "slice.cpp"
#endif

#endif
