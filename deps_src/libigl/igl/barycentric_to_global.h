// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_BARYCENTRIC2GLOBAL_H
#define IGL_BARYCENTRIC2GLOBAL_H
#include <igl/igl_inline.h>

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl 
{
  // Converts barycentric coordinates in the embree form to 3D coordinates
  // Embree stores barycentric coordinates as triples: fid, bc1, bc2
  // fid is the id of a face, bc1 is the displacement of the point wrt the 
  // first vertex v0 and the edge v1-v0. Similarly, bc2 is the displacement
  // wrt v2-v0.
  // 
  // Input:
  // V:  #Vx3 Vertices of the mesh
  // F:  #Fxe Faces of the mesh
  // bc: #Xx3 Barycentric coordinates, one row per point
  //
  // Output:
  // #X: #Xx3 3D coordinates of all points in bc
  template <typename Scalar, typename Index>
  IGL_INLINE Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> 
    barycentric_to_global(
      const Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> & V, 
      const Eigen::Matrix<Index,Eigen::Dynamic,Eigen::Dynamic>   & F, 
      const Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic>  & bc);
}

#ifndef IGL_STATIC_LIBRARY
#  include "barycentric_to_global.cpp"
#endif

#endif
