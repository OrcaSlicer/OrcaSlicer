// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FIND_H
#define IGL_FIND_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <vector>
namespace igl
{
  /// Find the non-zero entries and there respective indices in a sparse matrix.
  /// Like matlab's [I,J,V] = find(X)
  ///
  /// @tparam T  should be a eigen sparse matrix primitive type like int or double
  /// @param[in] X  m by n matrix whose entries are to be found 
  /// @param[out] I  nnz vector of row indices of non zeros entries in X
  /// @param[out] J  nnz vector of column indices of non zeros entries in X
  /// @param[out] V  nnz vector of type T non-zeros entries in X
  ///
  template <
    typename T, 
    typename DerivedI, 
    typename DerivedJ,
    typename DerivedV>
  IGL_INLINE void find(
    const Eigen::SparseMatrix<T>& X,
    Eigen::DenseBase<DerivedI> & I,
    Eigen::DenseBase<DerivedJ> & J,
    Eigen::DenseBase<DerivedV> & V);
  /// \overload
  template < typename T>
  IGL_INLINE std::vector<Eigen::Triplet<T> > find(
    const Eigen::SparseMatrix<T>& X);
  /// \overload
  template <
    typename DerivedX,
    typename DerivedI, 
    typename DerivedJ,
    typename DerivedV>
  IGL_INLINE void find(
    const Eigen::DenseBase<DerivedX>& X,
    Eigen::PlainObjectBase<DerivedI> & I,
    Eigen::PlainObjectBase<DerivedJ> & J,
    Eigen::PlainObjectBase<DerivedV> & V);
  /// \overload
  ///
  /// @param[out] I  nnz vector of indices into vectorization of X
  template <
    typename DerivedX,
    typename DerivedI>
  IGL_INLINE void find(
    const Eigen::DenseBase<DerivedX>& X,
    Eigen::PlainObjectBase<DerivedI> & I);
  /// \overload
  template <typename T>
  IGL_INLINE void find(
    const Eigen::SparseVector<T>& X,
    Eigen::Matrix<int,Eigen::Dynamic,1> & I,
    Eigen::Matrix<T,Eigen::Dynamic,1> & V);
  /// \overload
  /// \brief This overload facilitates the use of bool masks for Eigen slicing
  /// @tparam RowsAtCompileTime number of rows in M at compile time
  /// @return indices of true entries in M
  ///
  /// ##### Example
  ///
  /// ```cpp
  /// igl::slice_mask(A,igl::find(M),igl::find(N),B);
  /// // Is the same as
  /// B = A(igl::find(M),igl::find(N));
  /// ```
  ///
  /// \see slice_mask
  template <int RowsAtCompileTime, int MaxRowsAtCompileTime>
  IGL_INLINE std::vector<int> find(
    const Eigen::Array<bool,RowsAtCompileTime,1,0,MaxRowsAtCompileTime,1> & M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "find.cpp"
#endif

#endif
