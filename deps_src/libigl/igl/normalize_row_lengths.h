// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NORMALIZE_ROW_LENGTHS_H
#define IGL_NORMALIZE_ROW_LENGTHS_H
#include "igl_inline.h"
#include <Eigen/Core>

// History:
// March 24, 2012: Alec changed function name from normalize_rows to
//   normalize_row_lengths to avoid confusion with normalize_row_sums

namespace igl
{
  // Obsolete: just use A.rowwise().normalize() or B=A.rowwise().normalized();
  //
  // Normalize the rows in A so that their lengths are each 1 and place the new
  // entries in B
  // Inputs:
  //   A  #rows by k input matrix
  // Outputs:
  //   B  #rows by k input matrix, can be the same as A
  template <typename DerivedV>
  IGL_INLINE void normalize_row_lengths(
   const Eigen::PlainObjectBase<DerivedV>& A,
   Eigen::PlainObjectBase<DerivedV> & B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "normalize_row_lengths.cpp"
#endif

#endif
