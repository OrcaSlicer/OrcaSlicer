// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CUT_MESH
#define IGL_CUT_MESH
#include "igl_inline.h"

#include <Eigen/Core>
#include <vector>

namespace igl
{
  // Given a mesh and a list of edges that are to be cut, the function
  // generates a new disk-topology mesh that has the cuts at its boundary.
  //
  // Todo: this combinatorial operation should not depend on the vertex
  // positions V.
  //
  // Known issues: Assumes mesh is edge-manifold.
  //
  // Inputs:
  //   V  #V by 3 list of the vertex positions
  //   F  #F by 3 list of the faces (must be triangles)
  //   VF  #V list of lists of incident faces (adjacency list), e.g.  as
  //     returned by igl::vertex_triangle_adjacency
  //   VFi  #V list of lists of index of incidence within incident faces listed
  //     in VF, e.g. as returned by igl::vertex_triangle_adjacency
  //   TT  #F by 3 triangle to triangle adjacent matrix (e.g. computed via
  //     igl:triangle_triangle_adjacency)
  //   TTi  #F by 3 adjacent matrix, the element i,j is the id of edge of the
  //     triangle TT(i,j) that is adjacent with triangle i (e.g. computed via
  //     igl:triangle_triangle_adjacency)
  //   V_border  #V by 1 list of booleans, indicating if the corresponging
  //     vertex is at the mesh boundary, e.g. as returned by
  //     igl::is_border_vertex
  //   cuts  #F by 3 list of boolean flags, indicating the edges that need to
  //     be cut (has 1 at the face edges that are to be cut, 0 otherwise)
  // Outputs:
  //   Vcut  #V by 3 list of the vertex positions of the cut mesh. This matrix
  //     will be similar to the original vertices except some rows will be
  //     duplicated.
  //   Fcut  #F by 3 list of the faces of the cut mesh(must be triangles). This
  //     matrix will be similar to the original face matrix except some indices
  //     will be redirected to point to the newly duplicated vertices.
  //
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename VFType, 
    typename DerivedTT, 
    typename DerivedC>
  IGL_INLINE void cut_mesh(
    const Eigen::PlainObjectBase<DerivedV> &V,
    const Eigen::PlainObjectBase<DerivedF> &F,
    const std::vector<std::vector<VFType> >& VF,
    const std::vector<std::vector<VFType> >& VFi,
    const Eigen::PlainObjectBase<DerivedTT>& TT,
    const Eigen::PlainObjectBase<DerivedTT>& TTi,
    const std::vector<bool> &V_border,
    const Eigen::PlainObjectBase<DerivedC> &cuts,
    Eigen::PlainObjectBase<DerivedV> &Vcut,
    Eigen::PlainObjectBase<DerivedF> &Fcut);
  //Wrapper of the above with only vertices and faces as mesh input
  template <typename DerivedV, typename DerivedF, typename DerivedC>
  IGL_INLINE void cut_mesh(
    const Eigen::PlainObjectBase<DerivedV> &V,
    const Eigen::PlainObjectBase<DerivedF> &F,
    const Eigen::PlainObjectBase<DerivedC> &cuts,
    Eigen::PlainObjectBase<DerivedV> &Vcut,
    Eigen::PlainObjectBase<DerivedF> &Fcut);
};


#ifndef IGL_STATIC_LIBRARY
#include "cut_mesh.cpp"
#endif


#endif
