// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_REORDER_H
#define IGL_REORDER_H
#include "igl_inline.h"
#include <vector>
// For size_t
#include <stddef.h>
#include <cstdlib>

namespace igl
{
  // Act like matlab's Y = X(I) for std vectors
  // where I contains a vector of indices so that after,
  // Y[j] = X[I[j]] for index j
  // this implies that Y.size() == I.size()
  // X and Y are allowed to be the same reference
  template< class T >
  IGL_INLINE void reorder(
    const std::vector<T> & unordered,
    std::vector<size_t> const & index_map,
    std::vector<T> & ordered);
}

#ifndef IGL_STATIC_LIBRARY
#  include "reorder.cpp"
#endif

#endif
