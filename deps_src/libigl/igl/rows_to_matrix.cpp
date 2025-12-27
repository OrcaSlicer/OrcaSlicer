// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "rows_to_matrix.h"

#include <cassert>
#include <cstdio>

#include "max_size.h"
#include "min_size.h"

template <class Row, class Mat>
IGL_INLINE bool igl::rows_to_matrix(const std::vector<Row> & V,Mat & M)
{
  // number of columns
  int m = V.size();
  if(m == 0)
  {
    fprintf(stderr,"Error: rows_to_matrix() list is empty()\n");
    return false;
  }
  // number of rows
  int n = igl::min_size(V);
  if(n != igl::max_size(V))
  {
    fprintf(stderr,"Error: rows_to_matrix()"
      " list elements are not all the same size\n");
    return false;
  }
  assert(n != -1);
  // Resize output
  M.resize(m,n);

  // Loop over rows
  int i = 0;
  typename std::vector<Row>::const_iterator iter = V.begin();
  while(iter != V.end())
  {
    M.row(i) = V[i];
    // increment index and iterator
    i++;
    iter++;
  }

  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
