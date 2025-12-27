// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_PROJECT_H
#define IGL_OPENGL2_PROJECT_H
#include "../igl_inline.h"
#include <Eigen/Dense>
namespace igl
{
  namespace opengl2
  {
    // Wrapper for gluProject that uses the current GL_MODELVIEW_MATRIX,
    // GL_PROJECTION_MATRIX, and GL_VIEWPORT
    // Inputs:
    //   obj*  3D objects' x, y, and z coordinates respectively
    // Outputs:
    //   win*  pointers to screen space x, y, and z coordinates respectively
    // Returns return value of gluProject call
    IGL_INLINE int project(
      const double objX,
      const double objY,
      const double objZ,
      double* winX,
      double* winY,
      double* winZ);
    // Eigen wrapper
    template <typename Derivedobj, typename Derivedwin>
    IGL_INLINE int project(
      const Eigen::PlainObjectBase<Derivedobj> & obj,
      Eigen::PlainObjectBase<Derivedwin> & win);
    // Eigen wrapper  with return
    template <typename Derivedobj>
    IGL_INLINE Derivedobj project(
      const Eigen::PlainObjectBase<Derivedobj> & obj);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "project.cpp"
#endif

#endif

