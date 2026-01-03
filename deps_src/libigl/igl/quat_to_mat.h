// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUAT_TO_MAT_H
#define IGL_QUAT_TO_MAT_H
#include "igl_inline.h"
// Name history:
//   quat2mat  until 16 Sept 2011
namespace igl
{
  /// Convert a quaternion to a 4x4 matrix
  /// A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  /// such that q = x*i + y*j + z*k + w
  ///
  /// @param[in] quat  pointer to four elements of quaternion (x,y,z,w)  
  /// @param[out] mat  pointer to 16 elements of matrix
  template <typename Q_type>
  IGL_INLINE void quat_to_mat(const Q_type * quat, Q_type * mat);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quat_to_mat.cpp"
#endif

#endif
