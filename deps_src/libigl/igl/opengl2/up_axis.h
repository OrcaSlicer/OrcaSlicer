// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_UP_AXIS_H
#define IGL_OPENGL2_UP_AXIS_H 
#include "../igl_inline.h"
namespace igl
{
  namespace opengl2
  {
    // Determines the up axis or depth axis of the current gl matrix
    // Outputs:
    //   x  pointer to x-coordinate in scene coordinates of the un-normalized
    //     up axis 
    //   y  pointer to y-coordinate in scene coordinates of the un-normalized
    //     up axis 
    //   z  pointer to z-coordinate in scene coordinates of the un-normalized
    //     up axis
      //   mv pointer to modelview matrix
    //
    // Note: Up axis is returned *UN-normalized*
    IGL_INLINE void up_axis(double * x, double * y, double * z);
    IGL_INLINE void up_axis(const double * mv, double * x, double * y, double * z);
  }
};

#ifndef IGL_STATIC_LIBRARY
#  include "up_axis.cpp"
#endif
#endif


