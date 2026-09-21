// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NEXT_FILENAME_H
#define IGL_NEXT_FILENAME_H
#include "igl_inline.h"
#include <string>
namespace igl
{
  /// Find the file with the first filename of the form
  /// "prefix%0[zeros]dsuffix"
  /// 
  /// @param[in] prefix  path to containing dir and filename prefix
  /// @param[in] zeros number of leading zeros as if digit printed with printf
  /// @param[in] suffix  suffix of filename and extension (should include dot)
  /// @param[out] next  path to next file
  /// @return true if found, false if exceeding range in zeros
  IGL_INLINE bool next_filename(
    const std::string & prefix, 
    const int zeros,
    const std::string & suffix,
    std::string & next);
}

#ifndef IGL_STATIC_LIBRARY
#  include "next_filename.cpp"
#endif

#endif

