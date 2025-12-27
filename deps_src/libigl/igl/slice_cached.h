// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SLICE_CACHED_H
#define IGL_SLICE_CACHED_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{  

  // Act like the matlab X(row_indices,col_indices) operator, where row_indices,
  // col_indices are non-negative integer indices. This is a fast version of
  // igl::slice that can analyze and store the sparsity structure. It is slower
  // at the irst evaluation (slice_cached_precompute), but faster on the
  // subsequent ones.
  // 
  // Inputs:
  //   X  m by n matrix
  //   R  list of row indices
  //   C  list of column indices
  //   
  // Output:
  //   Y  #R by #C matrix
  //   data Temporary data used by slice_cached to repeat this operation
  //
  // Usage:
  //
  // // Construct and slice up Laplacian
  // SparseMatrix<double> L,L_sliced;
  // igl::cotmatrix(V,F,L);

  // // Normal igl::slice call
  // igl::slice(L,in,in,L_in_in);

  // // Fast version
  // static VectorXi data; // static or saved in a global state
  // if (data.size() == 0)
  //   igl::slice_cached_precompute(L,in,in,data,L_sliced);
  // else
  //   igl::slice_cached(L,data,L_sliced);

template <typename TX, typename TY, typename DerivedI>
IGL_INLINE void slice_cached_precompute(
  const Eigen::SparseMatrix<TX>& X,
  const Eigen::Matrix<int,Eigen::Dynamic,1> & R,
  const Eigen::Matrix<int,Eigen::Dynamic,1> & C,
  Eigen::MatrixBase<DerivedI>& data,
  Eigen::SparseMatrix<TY>& Y
  );

template <typename TX, typename TY, typename DerivedI>
IGL_INLINE void slice_cached(
  const Eigen::SparseMatrix<TX>& X,
  const Eigen::MatrixBase<DerivedI>& data,
  Eigen::SparseMatrix<TY>& Y
  );
}

#ifndef IGL_STATIC_LIBRARY
#  include "slice_cached.cpp"
#endif

#endif
