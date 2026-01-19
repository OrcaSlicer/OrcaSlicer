// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LINE_SEGMENT_IN_RECTANGLE_H
#define IGL_LINE_SEGMENT_IN_RECTANGLE_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Determine whether a line segment overlaps with a rectangle.
  //
  // Inputs:
  //   s  source point of line segment
  //   d  dest point of line segment
  //   A  first corner of rectangle
  //   B  opposite corner of rectangle
  // Returns true if line segment is at all inside rectangle
  IGL_INLINE bool line_segment_in_rectangle(
    const Eigen::Vector2d & s,
    const Eigen::Vector2d & d,
    const Eigen::Vector2d & A,
    const Eigen::Vector2d & B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "line_segment_in_rectangle.cpp"
#endif

#endif
