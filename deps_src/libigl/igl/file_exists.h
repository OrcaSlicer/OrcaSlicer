// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FILE_EXISTS_H
#define IGL_FILE_EXISTS_H
#include "igl_inline.h"
#include <string>
namespace igl
{
  /// Check if a file or directory exists like PHP's file_exists function:
  ///
  /// http://php.net/manual/en/function.file-exists.php
  ///
  /// @param[in] filename  path to file
  /// @return true if file exists and is readable and false if file doesn't
  /// exist or *is not readable*
  IGL_INLINE bool file_exists(const std::string filename);
}

#ifndef IGL_STATIC_LIBRARY
#  include "file_exists.cpp"
#endif

#endif
