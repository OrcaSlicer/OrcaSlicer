// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SHORTEST_EDGE_AND_MIDPOINT_H
#define IGL_SHORTEST_EDGE_AND_MIDPOINT_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Cost and placement function compatible with igl::decimate. The cost of
  /// collapsing an edge is its length (prefer to collapse short edges) and the
  /// placement strategy for the new vertex is the midpoint of the collapsed
  /// edge.
  ///
  /// @param[in] e  index into E of edge to be considered for collapse
  /// @param[in] V  #V by dim list of vertex positions
  /// @param[in] F  #F by 3 list of faces (ignored)
  /// @param[in] E  #E by 2 list of edge indices into V
  /// @param[in] EMAP  #F*3 list of half-edges indices into E (ignored)
  /// @param[in] EF  #E by 2 list of edge-face flaps into F (ignored)
  /// @param[in] EI  #E by 2 list of edge-face opposite corners (ignored)
  /// @param[out] cost  set to edge length
  /// @param[out] p  placed point set to edge midpoint
  IGL_INLINE void shortest_edge_and_midpoint(
    const int e,
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & /*F*/,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & /*EMAP*/,
    const Eigen::MatrixXi & /*EF*/,
    const Eigen::MatrixXi & /*EI*/,
    double & cost,
    Eigen::RowVectorXd & p);
}

#ifndef IGL_STATIC_LIBRARY
#  include "shortest_edge_and_midpoint.cpp"
#endif
#endif


