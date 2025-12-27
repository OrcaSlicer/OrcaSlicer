// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Yotam Gingold <yotam@yotamgingold.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SEAM_EDGES_H
#define IGL_SEAM_EDGES_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Finds all UV-space boundaries of a mesh.
  //
  // Inputs:
  //   V  #V by dim list of positions of the input mesh.
  //   TC  #TC by 2 list of 2D texture coordinates of the input mesh
  //   F  #F by 3 list of triange indices into V representing a
  //     manifold-with-boundary triangle mesh
  //   FTC  #F by 3 list of indices into TC for each corner
  // Outputs:
  //   seams  Edges where the forwards and backwards directions have different
  //     texture coordinates, as a #seams-by-4 matrix of indices. Each row is
  //     organized as [ forward_face_index, forward_face_vertex_index,
  //     backwards_face_index, backwards_face_vertex_index ] such that one side
  //     of the seam is the edge:
  //         F[ seams( i, 0 ), seams( i, 1 ) ], F[ seams( i, 0 ), (seams( i, 1 ) + 1) % 3 ]
  //     and the other side is the edge:
  //         F[ seams( i, 2 ), seams( i, 3 ) ], F[ seams( i, 2 ), (seams( i, 3 ) + 1) % 3 ]
  //   boundaries  Edges with only one incident triangle, as a #boundaries-by-2
  //     matrix of indices. Each row is organized as 
  //         [ face_index, face_vertex_index ]
  //     such that the edge is:
  //         F[ boundaries( i, 0 ), boundaries( i, 1 ) ], F[ boundaries( i, 0 ), (boundaries( i, 1 ) + 1) % 3 ]
  //   foldovers  Edges where the two incident triangles fold over each other
  //     in UV-space, as a #foldovers-by-4 matrix of indices.
  //     Each row is organized as [ forward_face_index, forward_face_vertex_index,
  //     backwards_face_index, backwards_face_vertex_index ]
  //     such that one side of the foldover is the edge:
  //       F[ foldovers( i, 0 ), foldovers( i, 1 ) ], F[ foldovers( i, 0 ), (foldovers( i, 1 ) + 1) % 3 ]
  //     and the other side is the edge:
  //       F[ foldovers( i, 2 ), foldovers( i, 3 ) ], F[ foldovers( i, 2 ), (foldovers( i, 3 ) + 1) % 3 ]
  template <
    typename DerivedV, 
    typename DerivedTC,
    typename DerivedF, 
    typename DerivedFTC,
    typename Derivedseams,
    typename Derivedboundaries,
    typename Derivedfoldovers>
  IGL_INLINE void seam_edges(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedTC>& TC,
    const Eigen::PlainObjectBase<DerivedF>& F,
    const Eigen::PlainObjectBase<DerivedFTC>& FTC,
    Eigen::PlainObjectBase<Derivedseams>& seams,
    Eigen::PlainObjectBase<Derivedboundaries>& boundaries,
    Eigen::PlainObjectBase<Derivedfoldovers>& foldovers);
}
#ifndef IGL_STATIC_LIBRARY
#  include "seam_edges.cpp"
#endif
#endif
