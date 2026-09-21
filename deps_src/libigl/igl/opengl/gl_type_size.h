// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GL_TYPE_SIZE_H
#define IGL_OPENGL_GL_TYPE_SIZE_H
#include "../igl_inline.h"
#include "gl.h"

namespace igl
{
  namespace opengl
  {
    /// Return the number of bytes for a given OpenGL type 
    ///
    /// @param[in] type  enum value of opengl type
    /// @return size in bytes of type
    IGL_INLINE int gl_type_size(const GLenum type);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "gl_type_size.cpp"
#endif

#endif
