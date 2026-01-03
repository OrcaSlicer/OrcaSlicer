// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PATH_TO_EXECUTABLE_H
#define IGL_PATH_TO_EXECUTABLE_H
#include "igl_inline.h"
#include <string>
namespace igl
{
  /// Path to current executable.
  /// @return path as string
  /// \note Tested for Mac OS X
  IGL_INLINE std::string path_to_executable();
}
#ifndef IGL_STATIC_LIBRARY
#  include "path_to_executable.cpp"
#endif
#endif 
