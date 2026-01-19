// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_READABLE_H
#define IGL_IS_READABLE_H
#include "igl_inline.h"
namespace igl
{
  // Check if a file is reabable like PHP's is_readable function:
  // http://www.php.net/manual/en/function.is-readable.php
  // Input:
  //   filename  path to file
  // Returns true if file exists and is readable and false if file doesn't
  // exist or *is not readable*
  //
  // Note: Windows version will not check user or group ids
  IGL_INLINE bool is_readable(const char * filename);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_readable.cpp"
#endif

#endif
