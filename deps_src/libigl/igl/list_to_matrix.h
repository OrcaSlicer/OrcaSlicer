// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LIST_TO_MATRIX_H
#define IGL_LIST_TO_MATRIX_H
#include "igl_inline.h"
#include <vector>
#include <array>
#include <Eigen/Core>

namespace igl
{
  /// Convert a list (std::vector) of row vectors of the same length to a matrix
  ///
  /// @tparam T  type that can be safely cast to type in Mat via '='
  /// @tparam Mat  Matrix type, must implement:
  ///     .resize(m,n)
  ///     .row(i) = Row
  /// @param[in] V  a m-long list of vectors of size n
  /// @param[out] M  an m by n matrix
  /// @return true on success, false on errors
  template <typename T, typename Derived>
  IGL_INLINE bool list_to_matrix(
    const std::vector<std::vector<T > > & V,
    Eigen::PlainObjectBase<Derived>& M);
  /// \overload
  template <typename T, size_t N, typename Derived>
  IGL_INLINE bool list_to_matrix(
    const std::vector<std::array<T, N> > & V,
    Eigen::PlainObjectBase<Derived>& M);
  /// \overload
  /// \brief Vector version.
  template <typename T, typename Derived>
  IGL_INLINE bool list_to_matrix(const std::vector<T > & V,Eigen::PlainObjectBase<Derived>& M);
  /// Convert a list of row vectors of `n` or less to a matrix and pad on
  /// the right with `padding`.
  ///
  /// @param[in] V  a m-long list of vectors of size <=n
  /// @param[in] n  number of columns
  /// @param[in] padding  value to fill in from right for short rows
  /// @param[out] M  an m by n matrix
  template <typename T, typename Derived>
  IGL_INLINE bool list_to_matrix(
    const std::vector<std::vector<T > > & V,
    const int n,
    const T & padding,
    Eigen::PlainObjectBase<Derived>& M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "list_to_matrix.cpp"
#endif

#endif
