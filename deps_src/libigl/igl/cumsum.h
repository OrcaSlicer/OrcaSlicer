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
  /// Computes a cumulative sum of the columns of X, like matlab's `cumsum`.
  ///
  /// @tparam DerivedX  Type of matrix X
  /// @tparam DerivedY  Type of matrix Y
  /// @param[in] X  m by n Matrix to be cumulatively summed.
  /// @param[in] dim  dimension to take cumulative sum (1 or 2)
  /// @param[out] Y  m by n Matrix containing cumulative sum.
  ///
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void cumsum(
    const Eigen::MatrixBase<DerivedX > & X,
    const int dim,
    Eigen::PlainObjectBase<DerivedY > & Y);
  /// Computes a cumulative sum of the columns of [0;X]
  ///
  /// @param[in] X  m by n Matrix to be cumulatively summed.
  /// @param[in] dim  dimension to take cumulative sum (1 or 2)
  /// @param[in] zero_prefix whether to use zero prefix
  /// @param[out] Y  if zero_prefix == false
  ///     m by n Matrix containing cumulative sum
  ///   else
  ///     m+1 by n Matrix containing cumulative sum if dim=1
  ///     or 
  ///     m by n+1 Matrix containing cumulative sum if dim=2
  template <typename DerivedX, typename DerivedY>
  IGL_INLINE void cumsum(
    const Eigen::MatrixBase<DerivedX > & X,
    const int dim,
    const bool zero_prefix,
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

