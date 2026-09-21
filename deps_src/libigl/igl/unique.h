// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_UNIQUE_H
#define IGL_UNIQUE_H
#include "igl_inline.h"

#include <vector>
#include <Eigen/Core>
namespace igl
{
  /// Act like matlab's [C,IA,IC] = unique(X)
  ///
  /// @tparam T  comparable type T
  /// @param[in] A  #A vector of type T
  /// @param[out] C  #C vector of unique entries in A
  /// @param[out] IA  #C index vector so that C = A(IA);
  /// @param[out] IC  #A index vector so that A = C(IC);
  template <typename T>
  IGL_INLINE void unique(
    const std::vector<T> & A,
    std::vector<T> & C,
    std::vector<size_t> & IA,
    std::vector<size_t> & IC);
  /// \overload
  template <typename T>
  IGL_INLINE void unique(
    const std::vector<T> & A,
    std::vector<T> & C);
  /// \overload
  template <
    typename DerivedA,
    typename DerivedC,
    typename DerivedIA,
    typename DerivedIC>
  IGL_INLINE void unique(
      const Eigen::MatrixBase<DerivedA> & A,
      Eigen::PlainObjectBase<DerivedC> & C,
      Eigen::PlainObjectBase<DerivedIA> & IA,
      Eigen::PlainObjectBase<DerivedIC> & IC);
  /// \overload
  template <
    typename DerivedA,
    typename DerivedC>
  IGL_INLINE void unique(
      const Eigen::MatrixBase<DerivedA> & A,
      Eigen::PlainObjectBase<DerivedC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "unique.cpp"
#endif

#endif
