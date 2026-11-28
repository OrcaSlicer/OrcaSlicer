// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LOOK_AT_H
#define IGL_LOOK_AT_H
#include "igl_inline.h"

#include <Eigen/Dense>

namespace igl 
{
  /// Implementation of the deprecated gluLookAt function.
  ///
  /// @param[in] eye  3-vector of eye position
  /// @param[in] center  3-vector of center reference point
  /// @param[in] up  3-vector of up vector
  /// @param[out] R  4x4 rotation matrix
  ///
  template <
    typename Derivedeye,
    typename Derivedcenter,
    typename Derivedup,
    typename DerivedR >
  IGL_INLINE void look_at(
    const Eigen::MatrixBase<Derivedeye> & eye,
    const Eigen::MatrixBase<Derivedcenter> & center,
    const Eigen::MatrixBase<Derivedup> & up,
    Eigen::PlainObjectBase<DerivedR> & R);
}

#ifndef IGL_STATIC_LIBRARY
#  include "look_at.cpp"
#endif

#endif

