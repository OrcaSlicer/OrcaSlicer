// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2017 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPARSE_CACHED_H
#define IGL_SPARSE_CACHED_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  // Build a sparse matrix from list of indices and values (I,J,V), similarly to 
  // the sparse function in matlab. Divides the construction in two phases, one
  // for fixing the sparsity pattern, and one to populate it with values. Compared to
  // igl::sparse, this version is slower for the first time (since it requires a
  // precomputation), but faster to the subsequent evaluations.
  //
  // Templates:
  //   IndexVector  list of indices, value should be non-negative and should
  //     expect to be cast to an index. Must implement operator(i) to retrieve
  //     ith element
  //   ValueVector  list of values, value should be expect to be cast to type
  //     T. Must implement operator(i) to retrieve ith element
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Input:
  //   I  nnz vector of row indices of non zeros entries in X
  //   J  nnz vector of column indices of non zeros entries in X
  //   V  nnz vector of non-zeros entries in X
  //   Optional:
  //     m  number of rows
  //     n  number of cols
  // Outputs:
  //   X  m by n matrix of type T whose entries are to be found 
  //
  // Example:
  //   Eigen::SparseMatrix<double> A;
  //   std::vector<Eigen::Triplet<double> > IJV;
  //   buildA(IJV);
  //   if (A.rows() == 0)
  //   {
  //     A = Eigen::SparseMatrix<double>(rows,cols);
  //     igl::sparse_cached_precompute(IJV,A,A_data);
  //   }
  //   else
  //     igl::sparse_cached(IJV,s.A,s.A_data);

  template <typename DerivedI, typename Scalar>
  IGL_INLINE void sparse_cached_precompute(
    const Eigen::MatrixBase<DerivedI> & I,
    const Eigen::MatrixBase<DerivedI> & J,
    Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X
    );
  
  template <typename Scalar>
  IGL_INLINE void sparse_cached_precompute(
    const std::vector<Eigen::Triplet<Scalar> >& triplets,
    Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X
    );

  template <typename Scalar>
  IGL_INLINE void sparse_cached(
    const std::vector<Eigen::Triplet<Scalar> >& triplets,
    const Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X);

  template <typename DerivedV, typename Scalar>
  IGL_INLINE void sparse_cached(
    const Eigen::MatrixBase<DerivedV>& V,
    const Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X
    );
  
}

#ifndef IGL_STATIC_LIBRARY
#  include "sparse_cached.cpp"
#endif

#endif
