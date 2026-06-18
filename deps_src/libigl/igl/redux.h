// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2023 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REDUX_H
#define IGL_REDUX_H
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  /// Perform reductions on the rows or columns of a SparseMatrix. This is
  /// _similar_ to DenseBase::redux, but different in two important ways:
  ///  1. (unstored) Zeros are **not** "visited", however if the first element
  ///     in the column/row  does not appear in the first row/column then the
  ///     reduction is assumed to start with zero. In this way, "any", "all",
  ///     "count"(non-zeros) work as expected. This means it is **not** possible
  ///     to use this to count (implicit) zeros.
  ///  2. This redux is more powerful in the sense that A and B may have
  ///     different types. This makes it possible to count the number of
  ///     non-zeros in a SparseMatrix<bool> A into a VectorXi B.
  ///
  /// @param[in] A  m by n sparse matrix
  /// @param[in] dim  dimension along which to sum (1 or 2)
  /// @param[in] func  function handle with the prototype `X(Y a, I i, J j, Z b)` where a
  ///     is the running value, b is A(i,j)
  /// @param[out] S  n-long sparse vector (if dim == 1) or m-long sparse vector
  /// (if dim == 2)
  ///
  /// \see for_each
  template <typename AType, typename Func, typename DerivedB>
  inline void redux(
    const Eigen::SparseMatrix<AType> & A,
    const int dim,
    const Func & func,
    Eigen::PlainObjectBase<DerivedB> & B);
}

// Implementation

#include "for_each.h"

template <typename AType, typename Func, typename DerivedB>
inline void igl::redux(
  const Eigen::SparseMatrix<AType> & A,
  const int dim,
  const Func & func,
  Eigen::PlainObjectBase<DerivedB> & B)
{
  typedef typename Eigen::SparseMatrix<AType>::StorageIndex Index;
  assert((dim == 1 || dim == 2) && "dim must be 2 or 1");
  // Get size of input
  int m = A.rows();
  int n = A.cols();
  // resize output
  B = DerivedB::Zero(dim==1?n:m);
  const auto func_wrap = [&func,&B,&dim](const Index i, const Index j, const AType v)
  {
    if(dim == 1)
    {
      B(j) = i == 0? v : func(B(j),v);
    }else
    {
      B(i) = j == 0? v : func(B(i),v);
    }
  };
  for_each(A,func_wrap);
}


//#ifndef IGL_STATIC_LIBRARY
//#  include "redux.cpp"
//#endif
#endif
