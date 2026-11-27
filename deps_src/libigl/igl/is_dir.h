// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_DIR_H
#define IGL_IS_DIR_H
#include "igl_inline.h"
namespace igl
{
  /// Tells whether the given filename is a directory.
  /// Act like php's is_dir function
  /// http://php.net/manual/en/function.is-dir.php
  ///
  /// @param[in] filename  Path to the file. If filename is a relative filename, it will
  ///     be checked relative to the current working directory. 
  /// @return TRUE if the filename exists and is a directory, FALSE
  /// otherwise.
  IGL_INLINE bool is_dir(const char * filename);

}

#ifndef IGL_STATIC_LIBRARY
#  include "is_dir.cpp"
#endif

#endif
