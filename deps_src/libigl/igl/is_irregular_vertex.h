// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_IRREGULAR_VERTEX_H
#define IGL_IS_IRREGULAR_VERTEX_H
#include "igl_inline.h"

#include <Eigen/Core>
#include <vector>

namespace igl
{
  // Determine if a vertex is irregular, i.e. it has more than 6 (triangles)
  // or 4 (quads) incident edges. Vertices on the boundary are ignored.
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by 3[4] list of triangle[quads] indices
  // Returns #V vector of bools revealing whether vertices are singular
  //
  template <typename DerivedV, typename DerivedF>
  IGL_INLINE std::vector<bool> is_irregular_vertex(const Eigen::PlainObjectBase<DerivedV> &V, const Eigen::PlainObjectBase<DerivedF> &F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_irregular_vertex.cpp"
#endif

#endif
