// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ROTATE_BY_QUAT_H
#define IGL_ROTATE_BY_QUAT_H
#include "igl_inline.h"

namespace igl
{
  /// Compute rotation of a given vector/point by a quaternion
  /// A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  /// such that q = x*i + y*j + z*k + w
  ///
  /// @param[in] v  input 3d point/vector
  /// @param[in] q  input quaternion
  /// @param[out] out  result of rotation, allowed to be same as v
  template <typename Q_type>
  IGL_INLINE void rotate_by_quat(
    const Q_type *v,
    const Q_type *q, 
    Q_type *out);
};

#ifndef IGL_STATIC_LIBRARY
#  include "rotate_by_quat.cpp"
#endif

#endif
