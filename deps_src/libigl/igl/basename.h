// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BASENAME_H
#define IGL_BASENAME_H
#include "igl_inline.h"

#include <string>

namespace igl
{
  // Function like PHP's basename: /etc/sudoers.d --> sudoers.d
  // Input:
  //  path  string containing input path
  // Returns string containing basename (see php's basename)
  //
  // See also: dirname, pathinfo
  IGL_INLINE std::string basename(const std::string & path);
}

#ifndef IGL_STATIC_LIBRARY
#  include "basename.cpp"
#endif

#endif
