// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ORTHO_H
#define IGL_ORTHO_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl 
{
  /// Implementation of the deprecated glOrtho function.
  ///
  /// @param[in] left  coordinate of left vertical clipping plane
  /// @param[in] right  coordinate of right vertical clipping plane
  /// @param[in] bottom  coordinate of bottom vertical clipping plane
  /// @param[in] top  coordinate of top vertical clipping plane
  /// @param[in] nearVal  distance to near plane
  /// @param[in] farVal  distance to far plane
  /// @param[out] P  4x4 perspective matrix
  template < typename DerivedP>
  IGL_INLINE void ortho(
    const typename DerivedP::Scalar left,
    const typename DerivedP::Scalar right,
    const typename DerivedP::Scalar bottom,
    const typename DerivedP::Scalar top,
    const typename DerivedP::Scalar nearVal,
    const typename DerivedP::Scalar farVal,
    Eigen::PlainObjectBase<DerivedP> & P);
}

#ifndef IGL_STATIC_LIBRARY
#  include "ortho.cpp"
#endif

#endif
