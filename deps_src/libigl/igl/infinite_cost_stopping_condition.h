// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_INFINITE_COST_STOPPING_CONDITION_H
#define IGL_INFINITE_COST_STOPPING_CONDITION_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <set>
#include <functional>
namespace igl
{
  // Stopping condition function compatible with igl::decimate. The output
  // function handle will return true if cost of next edge is infinite.
  //
  // Inputs:
  //   cost_and_placement  handle being used by igl::collapse_edge
  // Outputs:
  //   stopping_condition
  //
  IGL_INLINE void infinite_cost_stopping_condition(
    const std::function<void(
      const int,
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      double &,
      Eigen::RowVectorXd &)> & cost_and_placement,
    std::function<bool(
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const std::set<std::pair<double,int> > &,
      const std::vector<std::set<std::pair<double,int> >::iterator > &,
      const Eigen::MatrixXd &,
      const int,
      const int,
      const int,
      const int,
      const int)> & stopping_condition);
  IGL_INLINE 
    std::function<bool(
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const std::set<std::pair<double,int> > &,
      const std::vector<std::set<std::pair<double,int> >::iterator > &,
      const Eigen::MatrixXd &,
      const int,
      const int,
      const int,
      const int,
      const int)> 
    infinite_cost_stopping_condition(
      const std::function<void(
        const int,
        const Eigen::MatrixXd &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        const Eigen::VectorXi &,
        const Eigen::MatrixXi &,
        const Eigen::MatrixXi &,
        double &,
        Eigen::RowVectorXd &)> & cost_and_placement);
}

#ifndef IGL_STATIC_LIBRARY
#  include "infinite_cost_stopping_condition.cpp"
#endif
#endif


