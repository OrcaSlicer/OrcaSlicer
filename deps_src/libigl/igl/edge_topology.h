// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGE_TOPOLOGY_H
#define IGL_EDGE_TOPOLOGY_H

#include "igl_inline.h"

#include <Eigen/Core>
#include <vector>

namespace igl 
{
  // Initialize Edges and their topological relations (assumes an edge-manifold
  // mesh)
  //
  // Inputs:
  //   V  #V by dim list of mesh vertex positions (unused)
  //   F  #F by 3 list of triangle indices into V
  // Outputs:
  //   EV  #Ex2 matrix storing the edge description as pair of indices to
  //       vertices
  //   FE  #Fx3 matrix storing the Triangle-Edge relation
  //   EF  #Ex2 matrix storing the Edge-Triangle relation
  //
  // TODO: This seems to be a inferior duplicate of edge_flaps.h:
  //   - unused input parameter V
  //   - roughly 2x slower than edge_flaps
  //   - outputs less information: edge_flaps reveals corner opposite edge
  //   - FE uses non-standard and ambiguous order: FE(f,c) is merely an edge
  //     incident on corner c of face f. In contrast, edge_flaps's EMAP(f,c)
  //     reveals the edge _opposite_ corner c of face f
template <typename DerivedV, typename DerivedF>
  IGL_INLINE void edge_topology(
    const Eigen::PlainObjectBase<DerivedV>& V,
    const Eigen::PlainObjectBase<DerivedF>& F, 
    Eigen::MatrixXi& EV, 
    Eigen::MatrixXi& FE, 
    Eigen::MatrixXi& EF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "edge_topology.cpp"
#endif

#endif
