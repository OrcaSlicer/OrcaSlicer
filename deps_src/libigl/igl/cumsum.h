// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CUMSUM_H
#define IGL_CUMSUM_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  // Computes a cumulative sum of the columns of X, like matlab's `cumsum`.
  //
  // Templates:
  //   DerivedX  Type of matrix X
  //   DerivedY  Type of matrix Y
  // Inputs:
  //   X  m by n Matrix to be cumulatively summed.
  //   dim  dimension to take cumulative sum (1 or 2)
  // Output:
  //   Y  m by n Matrix containing cumulative sum.
  //
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void cumsum(
    const Eigen::MatrixBase<DerivedX > & X,
    const int dim,
    Eigen::PlainObjectBase<DerivedY > & Y);
  //template <typename DerivedX, typename DerivedY>
  //IGL_INLINE void cumsum(
  //  const Eigen::MatrixBase<DerivedX > & X,
  //  Eigen::PlainObjectBase<DerivedY > & Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cumsum.cpp"
#endif

#endif

