// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SUM_H
#define IGL_SUM_H
#include "igl_inline.h"
#include <Eigen/Sparse>

namespace igl
{
  // Note: If your looking for dense matrix matlab like sum for eigen matrics
  // just use:
  //   M.colwise().sum() or M.rowwise().sum()
  // 

  // Sum the columns or rows of a sparse matrix
  // Templates:
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Inputs:
  //   X  m by n sparse matrix
  //   dim  dimension along which to sum (1 or 2)
  // Output:
  //   S  n-long sparse vector (if dim == 1) 
  //   or
  //   S  m-long sparse vector (if dim == 2)
  template <typename T>
  IGL_INLINE void sum(
    const Eigen::SparseMatrix<T>& X, 
    const int dim,
    Eigen::SparseVector<T>& S);
  // Sum is "conducted" in the type of DerivedB::Scalar 
  template <typename AType, typename DerivedB>
  IGL_INLINE void sum(
    const Eigen::SparseMatrix<AType> & A, 
    const int dim,
    Eigen::PlainObjectBase<DerivedB>& B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "sum.cpp"
#endif

#endif
