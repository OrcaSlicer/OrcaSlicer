// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_FILE_H
#define IGL_IS_FILE_H
#include "igl_inline.h"
namespace igl
{
  /// Tells whether the given filename is a regular file.
  /// Act like php's is_file function
  /// http://php.net/manual/en/function.is-file.php
  ///
  /// @param[in] filename  Path to the file. If filename is a relative filename, it will
  ///     be checked relative to the current working directory. 
  /// @return TRUE if the filename exists and is a regular file, FALSE
  /// otherwise.
  IGL_INLINE bool is_file(const char * filename);

}

#ifndef IGL_STATIC_LIBRARY
#  include "is_file.cpp"
#endif

#endif
