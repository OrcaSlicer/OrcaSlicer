// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_VIEW_AXIS_H
#define IGL_OPENGL2_VIEW_AXIS_H 
#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace opengl2
  {
    // Determines the view axis or depth axis of the current gl matrix
    // Inputs:
    //   mv pointer to modelview matrix
    // Outputs:
    //   x  pointer to x-coordinate in scene coordinates of the un-normalized
    //     viewing axis 
    //   y  pointer to y-coordinate in scene coordinates of the un-normalized
    //     viewing axis 
    //   z  pointer to z-coordinate in scene coordinates of the un-normalized
    //     viewing axis
    //
    // Note: View axis is returned *UN-normalized*
    IGL_INLINE void view_axis(const double * mv, double * x, double * y, double * z);
    // Extract mv from current GL state.
    IGL_INLINE void view_axis(double * x, double * y, double * z);
    template <typename DerivedV>
    IGL_INLINE void view_axis(Eigen::PlainObjectBase<DerivedV> & V);
  }
};


#ifndef IGL_STATIC_LIBRARY
#  include "view_axis.cpp"
#endif

#endif

