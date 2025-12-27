// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REQUIRES_ARG_H
#define IGL_REQUIRES_ARG_H
#include "../igl_inline.h"
#include <mex.h>
namespace igl
{
  namespace matlab
  {
    // Simply throw an error if (i+1)<rhs 
    //
    // Input:
    //   i  index of current arg
    //   nrhs  total number of args
    //   name of current arg
    IGL_INLINE void requires_arg(const int i, const int nrhs, const char *name);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "requires_arg.cpp"
#endif
#endif

