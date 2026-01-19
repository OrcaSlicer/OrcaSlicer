// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ANY_H
#define IGL_ANY_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Sparse>
namespace igl
{
  // For Dense matrices use: A.rowwise().any() or A.colwise().any()
  //
  // Inputs:
  //   A  m by n sparse matrix
  //   dim  dimension along which to check for any (1 or 2)
  // Output:
  //   B  n-long vector (if dim == 1) 
  //   or
  //   B  m-long vector (if dim == 2)
  //
  template <typename AType, typename DerivedB>
  IGL_INLINE void any(
    const Eigen::SparseMatrix<AType> & A, 
    const int dim,
    Eigen::PlainObjectBase<DerivedB>& B);
}
#ifndef IGL_STATIC_LIBRARY
#  include "any.cpp"
#endif
#endif

