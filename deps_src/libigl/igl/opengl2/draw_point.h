// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_POINT_H
#define IGL_OPENGL2_DRAW_POINT_H
#include "../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace opengl2
  {

    //double POINT_COLOR[3] = {239./255.,213./255.,46./255.};
    // Draw a nice looking, colored dot at a given point in 3d.
    //
    // Note: expects that GL_CURRENT_COLOR is set with the desired foreground color
    // 
    // Inputs:
    //   x  x-coordinate of point, modelview coordinates
    //   y  y-coordinate of point, modelview coordinates
    //   z  z-coordinate of point, modelview coordinates
    //   requested_r  outer-most radius of dot {7}, measured in screen space pixels
    //   selected  fills inner circle with black {false}
    // Asserts that requested_r does not exceed 0.5*GL_POINT_SIZE_MAX
    IGL_INLINE void draw_point(
      const double x,
      const double y,
      const double z,
      const double requested_r = 7,
      const bool selected = false);
    template <typename DerivedP>
    IGL_INLINE void draw_point(
      const Eigen::PlainObjectBase<DerivedP> & P,
      const double requested_r = 7,
      const bool selected = false);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "draw_point.cpp"
#endif

#endif
