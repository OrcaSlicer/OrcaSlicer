// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_PRINT_GL_GET_H
#define IGL_OPENGL2_PRINT_GL_GET_H
#include "gl.h"
#include "../igl_inline.h"

namespace igl
{
  namespace opengl2
  {
    // Prints the value of pname found by issuing glGet*(pname,*)
    // Inputs:
    //   pname  enum key to gl parameter
    IGL_INLINE void print_gl_get(GLenum pname);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "print_gl_get.cpp"
#endif

#endif
