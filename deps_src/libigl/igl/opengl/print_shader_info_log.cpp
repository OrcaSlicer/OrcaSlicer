// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "print_shader_info_log.h"

#include <cstdio>
#include <stdlib.h>
// Copyright Denis Kovacs 4/10/08
IGL_INLINE void igl::opengl::print_shader_info_log(const GLuint obj)
{
  GLint infologLength = 0;
  GLint charsWritten  = 0;
  char *infoLog;
  
  // Get shader info log from opengl
  glGetShaderiv(obj, GL_INFO_LOG_LENGTH,&infologLength);
  // Only print if there is something in the log
  if (infologLength > 0)
  {
    infoLog = (char *)malloc(infologLength);
    glGetShaderInfoLog(obj, infologLength, &charsWritten, infoLog);
    printf("%s\n",infoLog);
    free(infoLog);
  }
}
