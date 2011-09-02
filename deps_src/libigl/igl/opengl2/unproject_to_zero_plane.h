// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_UNPROJECT_TO_ZERO_PLANE_H
#define IGL_OPENGL2_UNPROJECT_TO_ZERO_PLANE_H
#include "../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace opengl2
  {
  // Wrapper for gluUnproject that uses the current GL_MODELVIEW_MATRIX,
    // GL_PROJECTION_MATRIX, and GL_VIEWPORT to unproject a screen position
    // (winX,winY) to a 3d location at same depth as the current origin.
    // Inputs:
    //   win*  screen space x, y, and z coordinates respectively
    // Outputs:
    //   obj*  pointers to 3D objects' x, y, and z coordinates respectively
    // Returns return value of gluUnProject call
    IGL_INLINE void unproject_to_zero_plane(
      const double winX,
      const double winY,
      double* objX,
      double* objY,
      double* objZ);
    template <typename Derivedwin, typename Derivedobj>
    IGL_INLINE void unproject_to_zero_plane(
      const Eigen::PlainObjectBase<Derivedwin> & win,
      Eigen::PlainObjectBase<Derivedobj> & obj);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "unproject_to_zero_plane.cpp"
#endif

#endif

