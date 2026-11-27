// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MIN_SIZE_H
#define IGL_MIN_SIZE_H
#include "igl_inline.h"
#include <vector>

namespace igl
{
  /// Determine min size of lists in a vector
  /// @tparam T  some list type object that implements .size()
  /// @param[in] V  vector of list types T
  /// @return min .size() found in V, returns -1 if V is empty
  template <typename T>
  IGL_INLINE int min_size(const std::vector<T> & V);
}


#ifndef IGL_STATIC_LIBRARY
#  include "min_size.cpp"
#endif

#endif
