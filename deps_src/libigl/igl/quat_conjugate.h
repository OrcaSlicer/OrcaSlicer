// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUAT_CONJUGATE_H
#define IGL_QUAT_CONJUGATE_H
#include "igl_inline.h"

namespace igl
{
  // Compute conjugate of given quaternion
  // http://en.wikipedia.org/wiki/Quaternion#Conjugation.2C_the_norm.2C_and_reciprocal
  // A Quaternion, q, is defined here as an arrays of four scalars (x,y,z,w),
  // such that q = x*i + y*j + z*k + w
  // Inputs:
  //   q1  input quaternion
  // Outputs:
  //   out  result of conjugation, allowed to be same as input
  template <typename Q_type>
  IGL_INLINE void quat_conjugate(
    const Q_type *q1, 
    Q_type *out);
};

#ifndef IGL_STATIC_LIBRARY
#  include "quat_conjugate.cpp"
#endif

#endif
