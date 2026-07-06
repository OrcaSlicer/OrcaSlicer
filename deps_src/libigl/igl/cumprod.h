// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CUMPROD_H
#define IGL_CUMPROD_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Computes a cumulative product of the columns of X, like matlab's `cumprod`.
  ///
  /// @tparam DerivedX  Type of matrix X
  /// @tparam DerivedY  Type of matrix Y
  /// @param[in] X  m by n Matrix to be cumulatively multiplied.
  /// @param[in] dim  dimension to take cumulative product (1 or 2)
  /// @param[out] Y  m by n Matrix containing cumulative product.
  ///
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void cumprod(
    const Eigen::MatrixBase<DerivedX > & X,
    const int dim,
    Eigen::PlainObjectBase<DerivedY > & Y);
  //template <typename DerivedX, typename DerivedY>
  //IGL_INLINE void cumprod(
  //  const Eigen::MatrixBase<DerivedX > & X,
  //  Eigen::PlainObjectBase<DerivedY > & Y);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cumprod.cpp"
#endif

#endif


