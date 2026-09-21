// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_INTERSECT_H
#define IGL_INTERSECT_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Determine the intersect between two sets of coefficients using ==
  ///
  /// @tparam M  matrix type that implements indexing by global index M(i)
  /// @param[in] A  matrix of coefficients
  /// @param[in] B  matrix of coefficients
  /// @param[out] C  matrix of elements appearing in both A and B, C is always resized to
  ///   have a single column
  template <class M>
  IGL_INLINE void intersect(const M & A, const M & B, M & C);
  /// overload
  template <class M>
  IGL_INLINE M intersect(const M & A, const M & B);
}
#ifndef IGL_STATIC_LIBRARY
#include "intersect.cpp"
#endif
#endif
