// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "gl_type_size.h"
#include <cassert>

IGL_INLINE int igl::opengl::gl_type_size(const GLenum type)
{
  switch(type)
  {
    case GL_DOUBLE:
      return 8;
      break;
    case GL_FLOAT:
      return 4;
      break;
    case GL_INT:
      return 4;
      break;
    default:
      // should handle all other GL_[types]
      assert(false && "Implementation incomplete.");
      break;
  }
  return -1;
}
