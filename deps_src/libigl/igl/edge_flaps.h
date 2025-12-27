// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EDGE_FLAPS_H
#define IGL_EDGE_FLAPS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Determine "edge flaps": two faces on either side of a unique edge (assumes
  // edge-manifold mesh)
  //
  // Inputs:
  //   F  #F by 3 list of face indices
  //   E  #E by 2 list of edge indices into V.
  //   EMAP #F*3 list of indices into E, mapping each directed edge to unique
  //     unique edge in E
  // Outputs:
  //   EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  //     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  //     e=(j->i)
  //   EI  #E by 2 list of edge flap corners (see above).
  //
  // TODO: This seems to be a duplicate of edge_topology.h
  // igl::edge_topology(V,F,etEV,etFE,etEF);
  // igl::edge_flaps(F,efE,efEMAP,efEF,efEI);
  // [~,I] = sort(efE,2)
  // all( efE(sub2ind(size(efE),repmat(1:size(efE,1),2,1)',I)) == etEV )
  // all( efEF(sub2ind(size(efE),repmat(1:size(efE,1),2,1)',I)) == etEF )
  // all(efEMAP(sub2ind(size(F),repmat(1:size(F,1),3,1)',repmat([1 2 3],size(F,1),1))) == etFE(:,[2 3 1]))
  IGL_INLINE void edge_flaps(
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & EMAP,
    Eigen::MatrixXi & EF,
    Eigen::MatrixXi & EI);
  // Only faces as input
  IGL_INLINE void edge_flaps(
    const Eigen::MatrixXi & F,
    Eigen::MatrixXi & E,
    Eigen::VectorXi & EMAP,
    Eigen::MatrixXi & EF,
    Eigen::MatrixXi & EI);
}
#ifndef IGL_STATIC_LIBRARY
#  include "edge_flaps.cpp"
#endif

#endif
