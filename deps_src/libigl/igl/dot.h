// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DOT_H
#define IGL_DOT_H
#include "igl_inline.h"
namespace igl
{
  /// Computes out = dot(a,b)
  /// @param[in] a  left 3d vector
  /// @param[in] b  right 3d vector
  /// @return scalar dot product
  IGL_INLINE double dot(
    const double *a, 
    const double *b);
}

#ifndef IGL_STATIC_LIBRARY
#  include "dot.cpp"
#endif

#endif
