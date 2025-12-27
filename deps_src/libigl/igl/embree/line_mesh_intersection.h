// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_LINE_MESH_INTERSECTION_H
#define IGL_EMBREE_LINE_MESH_INTERSECTION_H
#include <igl/igl_inline.h>

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <vector>

namespace igl 
{
  namespace embree
  {
    // Project the point cloud V_source onto the triangle mesh
    // V_target,F_target. 
    // A ray is casted for every vertex in the direction specified by 
    // N_source and its opposite.
    //
    // Input:
    // V_source: #Vx3 Vertices of the source mesh
    // N_source: #Vx3 Normals of the point cloud
    // V_target: #V2x3 Vertices of the target mesh
    // F_target: #F2x3 Faces of the target mesh
    //
    // Output:
    // #Vx3 matrix of baricentric coordinate. Each row corresponds to 
    // a vertex of the projected mesh and it has the following format:
    // id b1 b2. id is the id of a face of the source mesh. b1 and b2 are 
    // the barycentric coordinates wrt the first two edges of the triangle
    // To convert to standard global coordinates, see barycentric_to_global.h
    template <typename ScalarMatrix, typename IndexMatrix>
    IGL_INLINE ScalarMatrix line_mesh_intersection
    (
     const ScalarMatrix & V_source,
     const ScalarMatrix  & N_source,
     const ScalarMatrix & V_target,
     const IndexMatrix  & F_target
     );
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "line_mesh_intersection.cpp"
#endif

#endif
