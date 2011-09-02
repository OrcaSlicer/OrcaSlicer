// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ANY_OF_H
#define IGL_ANY_OF_H
#include "igl_inline.h"
namespace igl
{
  // Wrapper for STL `any_of` for matrix types
  //
  // Inputs:
  //   S  matrix
  // Returns whether any entries are true
  //
  // Seems that Eigen (now) implements this for `Eigen::Array` 
  template <typename Mat>
  IGL_INLINE bool any_of(const Mat & S);
}
#ifndef IGL_STATIC_LIBRARY
#  include "any_of.cpp"
#endif
#endif
