// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_PRINT_PROGRAM_INFO_LOG_H
#define IGL_OPENGL_PRINT_PROGRAM_INFO_LOG_H
#include "../igl_inline.h"
#include "gl.h"

namespace igl
{
  namespace opengl
  {
    /// Print the information log for a program object
    ///
    /// @param[in] obj  OpenGL index of program to print info log about
    IGL_INLINE void print_program_info_log(const GLuint obj);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "print_program_info_log.cpp"
#endif

#endif
