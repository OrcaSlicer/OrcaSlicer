// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_WRITABLE_H
#define IGL_IS_WRITABLE_H
#include "igl_inline.h"
namespace igl
{
  /// Check if a file exists *and* is writable like PHP's is_writable function:
  /// http://www.php.net/manual/en/function.is-writable.php
  /// @param[in] filename  path to file
  /// @return true if file exists and is writable and false if file doesn't
  /// exist or *is not writable*
  ///
  /// \note Windows version will not test group and user id
  IGL_INLINE bool is_writable(const char * filename);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_writable.cpp"
#endif

#endif
