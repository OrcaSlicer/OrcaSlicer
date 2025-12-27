// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DIRNAME_H
#define IGL_DIRNAME_H
#include "igl_inline.h"

#include <string>

namespace igl
{
  // Function like PHP's dirname: /etc/passwd --> /etc, 
  // Input:
  //  path  string containing input path
  // Returns string containing dirname (see php's dirname)
  //
  // See also: basename, pathinfo
  IGL_INLINE std::string dirname(const std::string & path);
}

#ifndef IGL_STATIC_LIBRARY
#  include "dirname.cpp"
#endif

#endif
