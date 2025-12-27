// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_PER_VERTEX_POINT_TO_PLANE_QUADRICS_H
#define IGL_PER_VERTEX_POINT_TO_PLANE_QUADRICS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <tuple>
namespace igl
{
  // Compute quadrics per vertex of a "closed" triangle mesh (V,F). Rather than
  // follow the qslim paper, this implements the lesser-known _follow up_
  // "Simplifying Surfaces with Color and Texture using Quadric Error Metrics".
  // This allows V to be n-dimensional (where the extra coordiantes store
  // texture UVs, color RGBs, etc.
  //
  // Inputs:
  //   V  #V by n list of vertex positions. Assumes that vertices with
  //     infinite coordinates are "points at infinity" being used to close up
  //     boundary edges with faces. This allows special subspace quadrice for
  //     boundary edges: There should never be more than one "point at
  //     infinity" in a single triangle.
  //   F  #F by 3 list of triangle indices into V
  //   E  #E by 2 list of edge indices into V.
  //   EMAP #F*3 list of indices into E, mapping each directed edge to unique
  //     unique edge in E
  //   EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  //     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  //     e=(j->i)
  //   EI  #E by 2 list of edge flap corners (see above).
  // Outputs:
  //   quadrics  #V list of quadrics, where a quadric is a tuple {A,b,c} such
  //     that the quadratic energy of moving this vertex to position x is
  //     given by x'Ax - 2b + c
  //
  IGL_INLINE void per_vertex_point_to_plane_quadrics(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & EMAP,
    const Eigen::MatrixXi & EF,
    const Eigen::MatrixXi & EI,
    std::vector<
      std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> > & quadrics);
}
#ifndef IGL_STATIC_LIBRARY
#  include "per_vertex_point_to_plane_quadrics.cpp"
#endif
#endif
