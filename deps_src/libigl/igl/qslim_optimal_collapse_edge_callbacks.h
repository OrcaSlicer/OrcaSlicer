// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QSLIM_OPTIMAL_COLLAPSE_EDGE_CALLBACKS_H
#define IGL_QSLIM_OPTIMAL_COLLAPSE_EDGE_CALLBACKS_H
#include "igl_inline.h"
#include "decimate_callback_types.h"
#include <Eigen/Core>
#include <functional>
#include <vector>
#include <tuple>
#include <set>
namespace igl
{
  /// @private 
  ///
  /// Prepare callbacks for decimating edges using the qslim optimal placement
  /// metric.
  ///
  /// @param[in] E  #E by 2 list of working edges
  /// @param[in] quadrics  reference to list of working per vertex quadrics 
  /// @param[in] v1  working variable to maintain end point of collapsed edge
  /// @param[in] v2  working variable to maintain end point of collapsed edge
  /// @param[out] cost_and_placement  callback for evaluating cost of edge collapse and
  ///     determining placement of vertex (see collapse_edge)
  /// @param[out] pre_collapse  callback before edge collapse (see collapse_edge)
  /// @param[out] post_collapse  callback after edge collapse (see collapse_edge)
  ///
  /// See decimate.h for more details.
  ///
  /// \see collapse_edge
  IGL_INLINE void qslim_optimal_collapse_edge_callbacks(
    Eigen::MatrixXi & E,
    std::vector<
      std::tuple<
        Eigen::MatrixXd,
        Eigen::RowVectorXd,
        double> > & 
      quadrics,
    int & v1,
    int & v2,
    decimate_cost_and_placement_callback & cost_and_placement,
    decimate_pre_collapse_callback & pre_collapse,
    decimate_post_collapse_callback & post_collapse);
}
#ifndef IGL_STATIC_LIBRARY
#  include "qslim_optimal_collapse_edge_callbacks.cpp"
#endif
#endif
