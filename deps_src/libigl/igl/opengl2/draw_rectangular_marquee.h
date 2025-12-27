// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_RECTANGULAR_MARQUEE_H
#define IGL_OPENGL2_DRAW_RECTANGULAR_MARQUEE_H
#include "../igl_inline.h"
namespace igl
{
  namespace opengl2
  {
    // Draw a rectangular marquee (selection box) in screen space. This sets up
    // screen space using current viewport.
    //
    // Inputs:
    //   from_x  x coordinate of from point
    //   from_y  y coordinate of from point
    //   to_x  x coordinate of to point
    //   to_y  y coordinate of to point
    IGL_INLINE void draw_rectangular_marquee(
      const int from_x,
      const int from_y,
      const int to_x,
      const int to_y);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "draw_rectangular_marquee.cpp"
#endif

#endif
