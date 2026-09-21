// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COLLAPSE_LEAST_COST_EDGE_H
#define IGL_COLLAPSE_LEAST_COST_EDGE_H
#include "igl_inline.h"
#include "min_heap.h"
#include "decimate_callback_types.h"
#include "COLLAPSE_EDGE_NULL.h"
#include <Eigen/Core>
#include <vector>
namespace igl
{
  /// Collapse least-cost edge from a priority queue and update queue 
  ///
  /// See decimate.h for more details.
  ///
  /// @param[in] cost_and_placement  function computing cost of collapsing an edge and 3d
  ///     position where it should be placed:
  ///     cost_and_placement(V,F,E,EMAP,EF,EI,cost,placement);
  ///     **If the edges is collapsed** then this function will be called on all
  ///     edges of all faces previously incident on the endpoints of the
  ///     collapsed edge.
  ///  @param[in] pre_collapse  callback called with index of edge whose collapse is about
  ///     to be attempted. This function should return whether to **proceed**
  ///     with the collapse: returning true means "yes, try to collapse",
  ///     returning false means "No, consider this edge 'uncollapsable', behave
  ///     as if collapse_edge(e) returned false.
  ///  @param[in] post_collapse  callback called with index of edge whose collapse was
  ///     just attempted and a flag revealing whether this was successful.
  /// @param[in,out] V  #V by dim list of vertex positions, lesser index of E(e,:) will be set
  ///     to midpoint of edge.
  /// @param[in,out] F  #F by 3 list of face indices into V.
  /// @param[in,out] E  #E by 2 list of edge indices into V.
  /// @param[in,out] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in,out] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1)
  ///     e=(j->i)
  /// @param[in,out] EI  #E by 2 list of edge flap corners (see above).
  /// @param[in] Q  queue containing pairs of costs and edge indices and insertion "time"
  /// @param[in] EQ  #E list of "time" of last time pushed into Q
  /// @param[in] C  #E by dim list of stored placements
  /// @param[out] e  index into E of attempted collapsed edge. Set to -1 if Q is empty or
  ///               contains only infinite cost edges.
  /// @param[out] e1  index into E of edge collpased on left.
  /// @param[out] e2  index into E of edge collpased on right.
  /// @param[out] f1  index into F of face collpased on left.
  /// @param[out] f2  index into F of face collpased on right.
  ///
  /// \bug This function is not templated nicely and refactoring it and its
  /// dependencies to do so is non-trivial, see
  /// https://github.com/libigl/libigl/issues/2452
  IGL_INLINE bool collapse_least_cost_edge(
    const decimate_cost_and_placement_callback & cost_and_placement,
    const decimate_pre_collapse_callback       & pre_collapse,
    const decimate_post_collapse_callback      & post_collapse,
    Eigen::MatrixXd & V,
    Eigen::MatrixXi & F,
    Eigen::MatrixXi & E,
    Eigen::VectorXi & EMAP,
    Eigen::MatrixXi & EF,
    Eigen::MatrixXi & EI,
    igl::min_heap< std::tuple<double,int,int> > & Q,
    Eigen::VectorXi & EQ,
    Eigen::MatrixXd & C,
    int & e,
    int & e1,
    int & e2,
    int & f1,
    int & f2);
}

#ifndef IGL_STATIC_LIBRARY
#  include "collapse_least_cost_edge.cpp"
#endif
#endif

