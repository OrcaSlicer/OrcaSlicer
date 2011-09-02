// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "report_gl_error.h"
#include "../verbose.h"
#include <cstdio>

IGL_INLINE GLenum igl::opengl::report_gl_error(const std::string id)
{
  // http://stackoverflow.com/q/28485180/148668

  // gluErrorString was deprecated
  const auto gluErrorString = [](GLenum errorCode)->const char *
  {
    switch(errorCode)
    {
      default:
        return "unknown error code";
      case GL_NO_ERROR:
        return "no error";
      case GL_INVALID_ENUM:
        return "invalid enumerant";
      case GL_INVALID_VALUE:
        return "invalid value";
      case GL_INVALID_OPERATION:
        return "invalid operation";
#ifndef GL_VERSION_3_0
      case GL_STACK_OVERFLOW:
        return "stack overflow";
      case GL_STACK_UNDERFLOW:
        return "stack underflow";
      case GL_TABLE_TOO_LARGE:
        return "table too large";
#endif
      case GL_OUT_OF_MEMORY:
        return "out of memory";
#ifdef GL_EXT_framebuffer_object
      case GL_INVALID_FRAMEBUFFER_OPERATION_EXT:
        return "invalid framebuffer operation";
#endif
    }
  };

  GLenum err = glGetError();
  if(GL_NO_ERROR != err)
  {
    verbose("GL_ERROR: ");
    fprintf(stderr,"%s%s\n",id.c_str(),gluErrorString(err));
  }
  return err;
}

IGL_INLINE GLenum igl::opengl::report_gl_error()
{
  return igl::opengl::report_gl_error(std::string(""));
}

