// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "destroy_shader_program.h"
#include "report_gl_error.h"
#include <cstdio>

IGL_INLINE bool igl::opengl::destroy_shader_program(const GLuint id)
{
  // Don't try to destroy id == 0 (no shader program)
  if(id == 0)
  {
    fprintf(stderr,"Error: destroy_shader_program() id = %d"
      " but must should be positive\n",id);
    return false;
  }
  // Get each attached shader one by one and detach and delete it
  GLsizei count;
  // shader id
  GLuint s;
  do
  {
    // Try to get at most *1* attached shader
    glGetAttachedShaders(id,1,&count,&s);
    GLenum err = igl::opengl::report_gl_error();
    if (GL_NO_ERROR != err)
    {
      return false;
    }
    // Check that we actually got *1*
    if(count == 1)
    {
      // Detach and delete this shader
      glDetachShader(id,s);
      glDeleteShader(s);
    }
  }while(count > 0);
  // Now that all of the shaders are gone we can just delete the program
  glDeleteProgram(id);
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif

