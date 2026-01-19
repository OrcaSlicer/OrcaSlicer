// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_LOAD_SHADER_H 
#define IGL_OPENGL_LOAD_SHADER_H
#include "../igl_inline.h" 
#include "gl.h"
#include <string>

namespace igl
{
  namespace opengl
  {
    // Creates and compiles a shader from a given string
    //
    // Inputs:
    //   src  string containing GLSL shader code
    //   type  GLSL type of shader, one of:
    //     GL_VERTEX_SHADER
    //     GL_FRAGMENT_SHADER
    //     GL_GEOMETRY_SHADER
    // Returns  index id of the newly created shader, 0 on error
    // 
    // Will immediately return 0 if src is empty.
    IGL_INLINE GLuint load_shader(
      const std::string & src,const GLenum type);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "load_shader.cpp"
#endif

#endif
