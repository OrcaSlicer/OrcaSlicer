// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUAT_MULT_H
#define IGL_QUAT_MULT_H
#include "igl_inline.h"

namespace igl
{
  // Computes out = q1 * q2 with quaternion multiplication
  // A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  // such that q = x*i + y*j + z*k + w
  // Inputs:
  //   q1  left quaternion
  //   q2  right quaternion
  // Outputs:
  //   out  result of multiplication
  template <typename Q_type>
  IGL_INLINE void quat_mult(
    const Q_type *q1, 
    const Q_type *q2,
    Q_type *out);
};

#ifndef IGL_STATIC_LIBRARY
#  include "quat_mult.cpp"
#endif

#endif
