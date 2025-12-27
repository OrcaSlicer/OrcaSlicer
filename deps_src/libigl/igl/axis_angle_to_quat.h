// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_AXIS_ANGLE_TO_QUAT_H
#define IGL_AXIS_ANGLE_TO_QUAT_H
#include "igl_inline.h"

namespace igl
{
  // Convert axis angle representation of a rotation to a quaternion
  // A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  // such that q = x*i + y*j + z*k + w
  // Inputs:
  //   axis  3d vector
  //   angle  scalar
  // Outputs:
  //   quaternion
  template <typename Q_type>
  IGL_INLINE void axis_angle_to_quat(
    const Q_type *axis, 
    const Q_type angle,
    Q_type *out);
}

#ifndef IGL_STATIC_LIBRARY
#  include "axis_angle_to_quat.cpp"
#endif

#endif
