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
  /// Build a sparse matrix from list of indices and values (I,J,V), similarly to 
  /// the sparse function in matlab. Divides the construction in two phases, one
  /// for fixing the sparsity pattern, and one to populate it with values. Compared to
  /// igl::sparse, this version is slower for the first time (since it requires a
  /// precomputation), but faster to the subsequent evaluations.
  ///
  /// @param[in] I  nnz vector of row indices of non zeros entries in X
  /// @param[in] J  nnz vector of column indices of non zeros entries in X
  /// @param[out] data ?? vector of ??
  /// @param[out] X  m by n matrix of type T whose entries are to be found 
  ///
  /// #### Example:
  ///
  ///       Eigen::SparseMatrix<double> A;
  ///       std::vector<Eigen::Triplet<double> > IJV;
  ///       slim_buildA(IJV);
  ///       if (A.rows() == 0)
  ///       {
  ///         A = Eigen::SparseMatrix<double>(rows,cols);
  ///         igl::sparse_cached_precompute(IJV,A_data,A);
  ///       }
  ///       else
  ///         igl::sparse_cached(IJV,A_data,A);
  ///
  template <typename DerivedI, typename Scalar>
  IGL_INLINE void sparse_cached_precompute(
    const Eigen::MatrixBase<DerivedI> & I,
    const Eigen::MatrixBase<DerivedI> & J,
    Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X
    );
  /// \overload
  /// @param[in] triplets  nnz vector of triplets of non zeros entries in X
  template <typename Scalar>
  IGL_INLINE void sparse_cached_precompute(
    const std::vector<Eigen::Triplet<Scalar> >& triplets,
    Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X
    );
  /// Build a sparse matrix from cached list of data and values
  ///
  /// @param[in] triplets  nnz vector of triplets of non zeros entries in X
  /// @param[in] data ?? vector of ??
  /// @param[in,out] X  m by n matrix of type T whose entries are to be found 
  template <typename Scalar>
  IGL_INLINE void sparse_cached(
    const std::vector<Eigen::Triplet<Scalar> >& triplets,
    const Eigen::VectorXi& data,
    Eigen::SparseMatrix<Scalar>& X);
  /// \overload 
  /// @param[in] V #V list of values
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
