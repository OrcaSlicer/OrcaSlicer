// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_REPORT_GL_ERROR_H
#define IGL_OPENGL_REPORT_GL_ERROR_H
#include "../igl_inline.h"

// Hack to allow both opengl/ and opengl2 to use this (we shouldn't allow this)
#ifndef __gl_h_ 
#  include "gl.h"
#endif
#include <string>

namespace igl
{
  namespace opengl
  {
    /// Print last OpenGL error to stderr prefixed by specified id string
    /// @param[in] id   string to appear before any error msgs
    /// @return result of glGetError() 
    IGL_INLINE GLenum report_gl_error(const std::string id);
    /// \overload
    IGL_INLINE GLenum report_gl_error();
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "report_gl_error.cpp"
#endif

#endif
