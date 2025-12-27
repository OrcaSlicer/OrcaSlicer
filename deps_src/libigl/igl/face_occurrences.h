// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_FACE_OCCURRENCES
#define IGL_FACE_OCCURRENCES
#include "igl_inline.h"

#include <vector>
namespace igl
{
  // Count the occruances of each face (row) in a list of face indices
  // (irrespecitive of order)
  // Inputs:
  //   F  #F by simplex-size
  // Outputs
  //   C  #F list of counts
  // Known bug: triangles/tets only (where ignoring order still gives simplex)
  template <typename IntegerF, typename IntegerC>
  IGL_INLINE void face_occurrences(
    const std::vector<std::vector<IntegerF> > & F,
    std::vector<IntegerC> & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "face_occurrences.cpp"
#endif

#endif


