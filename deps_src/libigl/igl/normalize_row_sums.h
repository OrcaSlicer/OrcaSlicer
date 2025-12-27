// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NORMALIZE_ROW_SUMS_H
#define IGL_NORMALIZE_ROW_SUMS_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Normalize the rows in A so that their sums are each 1 and place the new
  // entries in B
  // Inputs:
  //   A  #rows by k input matrix
  // Outputs:
  //   B  #rows by k input matrix, can be the same as A
  //
  // Note: This is just calling an Eigen one-liner.
  template <typename DerivedA, typename DerivedB>
  IGL_INLINE void normalize_row_sums(
    const Eigen::MatrixBase<DerivedA>& A,
    Eigen::MatrixBase<DerivedB> & B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "normalize_row_sums.cpp"
#endif

#endif
