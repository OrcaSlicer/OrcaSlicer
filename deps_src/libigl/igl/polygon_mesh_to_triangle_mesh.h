// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_POLYGON_MESH_TO_TRIANGLE_MESH_H
#define IGL_POLYGON_MESH_TO_TRIANGLE_MESH_H
#include "igl_inline.h"

#ifndef IGL_NO_EIGEN
#  include <Eigen/Core>
#endif
#include <vector>

namespace igl
{
  // Triangulate a general polygonal mesh into a triangle mesh.
  //
  // Inputs:
  //   vF  list of polygon index lists
  // Outputs:
  //   F  eigen int matrix #F by 3
  //
  // Example:
  //   vector<vector<double > > vV;
  //   vector<vector<int > > vF;
  //   read_triangle_mesh("poly.obj",vV,vF);
  //   MatrixXd V;
  //   MatrixXi F;
  //   list_to_matrix(vV,V);
  //   triangulate(vF,F);
  template <typename Index, typename DerivedF>
  IGL_INLINE void polygon_mesh_to_triangle_mesh(
    const std::vector<std::vector<Index> > & vF,
    Eigen::PlainObjectBase<DerivedF>& F);
  template <typename DerivedP, typename DerivedF>
  IGL_INLINE void polygon_mesh_to_triangle_mesh(
    const Eigen::PlainObjectBase<DerivedP>& P,
    Eigen::PlainObjectBase<DerivedF>& F);
}

#ifndef IGL_STATIC_LIBRARY
#  include "polygon_mesh_to_triangle_mesh.cpp"
#endif

#endif
