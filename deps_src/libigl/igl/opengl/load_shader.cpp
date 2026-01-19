// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "load_shader.h"

// Copyright Denis Kovacs 4/10/08
#include "print_shader_info_log.h"
#include <cstdio>
IGL_INLINE GLuint igl::opengl::load_shader(
  const std::string & src,const GLenum type)
{
  if(src.empty())
  {
    return (GLuint) 0;
  }

  GLuint s = glCreateShader(type);
  if(s == 0)
  {
    fprintf(stderr,"Error: load_shader() failed to create shader.\n");
    return 0;
  }
  // Pass shader source string
  const char *c = src.c_str();
  glShaderSource(s, 1, &c, NULL);
  glCompileShader(s);
  // Print info log (if any)
  igl::opengl::print_shader_info_log(s);
  return s;
}
