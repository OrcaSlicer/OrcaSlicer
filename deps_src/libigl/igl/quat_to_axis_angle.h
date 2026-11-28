// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUAT_TO_AXIS_ANGLE_H
#define IGL_QUAT_TO_AXIS_ANGLE_H
#include "igl_inline.h"

namespace igl
{
  /// Convert quat representation of a rotation to axis angle
  /// A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  /// such that q = x*i + y*j + z*k + w
  ///
  /// @param[in] q quaternion
  /// @param[out] axis  3d vector
  /// @param[out] angle  scalar in radians
  template <typename Q_type>
  IGL_INLINE void quat_to_axis_angle(
    const Q_type *q,
    Q_type *axis, 
    Q_type & angle);
  /// \overload
  ///
  /// \fileinfo
  template <typename Q_type>
  IGL_INLINE void quat_to_axis_angle_deg(
    const Q_type *q,
    Q_type *axis, 
    Q_type & angle);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quat_to_axis_angle.cpp"
#endif

#endif

