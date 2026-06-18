// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MAT_TO_QUAT_H
#define IGL_MAT_TO_QUAT_H
#include "igl_inline.h"
namespace igl
{
  /// Convert a OpenGL (rotation) matrix to a quaternion
  ///
  /// @param[in] m  16-element opengl rotation matrix
  /// @param[out] q  4-element  quaternion (not normalized)
  template <typename Q_type>
  IGL_INLINE void mat4_to_quat(const Q_type * m, Q_type * q);
  /// \overload
  template <typename Q_type>
  IGL_INLINE void mat3_to_quat(const Q_type * m, Q_type * q);
}

#ifndef IGL_STATIC_LIBRARY
#  include "mat_to_quat.cpp"
#endif

#endif

