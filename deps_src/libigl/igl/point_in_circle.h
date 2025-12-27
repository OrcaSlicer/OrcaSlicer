// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POINT_IN_CIRCLE_H
#define IGL_POINT_IN_CIRCLE_H
#include "igl_inline.h"

namespace igl
{
  // Determine if 2d point is in a circle
  // Inputs:
  //   qx  x-coordinate of query point
  //   qy  y-coordinate of query point
  //   cx  x-coordinate of circle center
  //   cy  y-coordinate of circle center
  //   r  radius of circle
  // Returns true if query point is in circle, false otherwise
  IGL_INLINE bool point_in_circle(
    const double qx, 
    const double qy,
    const double cx, 
    const double cy,
    const double r);
}

#ifndef IGL_STATIC_LIBRARY
#  include "point_in_circle.cpp"
#endif

#endif
