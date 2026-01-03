// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MIN_HEAP_H
#define IGL_MIN_HEAP_H
#include <queue>
#include <vector>
#include <functional>
namespace igl
{
  /// Templated min heap (reverses sort order of std::priority_queue)
  /// @tparam T type of elements in heap
  template<class T> using min_heap = 
    std::priority_queue< T, std::vector<T >, std::greater<T > >;
}
#endif 

