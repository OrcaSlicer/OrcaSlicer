// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRIANGLES_FROM_STRIP_H
#define IGL_TRIANGLES_FROM_STRIP_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Create a list of triangles from a stream of indices along a strip.
  ///
  /// @param[in] S  #S list of indices
  /// @param[out] F  #S-2 by 3 list of triangle indices
  ///
  template <typename DerivedS, typename DerivedF>
  IGL_INLINE void triangles_from_strip(
    const Eigen::MatrixBase<DerivedS>& S,
    Eigen::PlainObjectBase<DerivedF>& F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "triangles_from_strip.cpp"
#endif

#endif

