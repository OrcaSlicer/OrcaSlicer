// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EXAMPLE_FUN_H
#define IGL_EXAMPLE_FUN_H

#include "igl_inline.h"

namespace igl
{
  // This is an example of a function, it takes a templated parameter and
  // shovels it into cout
  //
  // Templates:
  //   T  type that supports
  // Input:
  //   input  some input of a Printable type
  // Returns true for the sake of returning something
  template <typename Printable>
  IGL_INLINE bool example_fun(const Printable & input);
}

#ifndef IGL_STATIC_LIBRARY
#  include "example_fun.cpp"
#endif

#endif
