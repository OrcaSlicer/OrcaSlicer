// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPARSE_H
#define IGL_SPARSE_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Dense>
#include <Eigen/Sparse>
namespace igl
{
  // Build a sparse matrix from list of indices and values (I,J,V), functions
  // like the sparse function in matlab
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
  template <class IndexVector, class ValueVector, typename T>
  IGL_INLINE void sparse(
    const IndexVector & I,
    const IndexVector & J,
    const ValueVector & V,
    Eigen::SparseMatrix<T>& X);
  template <
    class IndexVectorI, 
    class IndexVectorJ, 
    class ValueVector, 
    typename T>
  IGL_INLINE void sparse(
    const IndexVectorI & I,
    const IndexVectorJ & J,
    const ValueVector & V,
    const size_t m,
    const size_t n,
    Eigen::SparseMatrix<T>& X);
  // THIS MAY BE SUPERSEDED BY EIGEN'S .sparseView Indeed it is.
  // Convert a full, dense matrix to a sparse one
  //
  // Templates:
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Input:
  //   D  m by n full, dense matrix
  // Output:
  //   X  m by n sparse matrix
  template <typename DerivedD, typename T>
  IGL_INLINE void sparse(
    const Eigen::PlainObjectBase<DerivedD>& D,
    Eigen::SparseMatrix<T>& X);
  // Wrapper with return
  template <typename DerivedD>
  IGL_INLINE Eigen::SparseMatrix<typename DerivedD::Scalar > sparse(
    const Eigen::PlainObjectBase<DerivedD>& D);

}

#ifndef IGL_STATIC_LIBRARY
#  include "sparse.cpp"
#endif

#endif
