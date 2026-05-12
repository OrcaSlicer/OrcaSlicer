// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "infinite_cost_stopping_condition.h"

IGL_INLINE void igl::infinite_cost_stopping_condition(
  const decimate_cost_and_placement_callback & cost_and_placement,
  decimate_stopping_condition_callback & stopping_condition)
{
  stopping_condition = 
    [&cost_and_placement]
    (
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & EMAP,
    const Eigen::MatrixXi & EF,
    const Eigen::MatrixXi & EI,
    const igl::min_heap< std::tuple<double,int,int> > & ,/*Q*/
    const Eigen::VectorXi &                             EQ,
    const Eigen::MatrixXd & /*C*/,
    const int e,
    const int /*e1*/,
    const int /*e2*/,
    const int /*f1*/,
    const int /*f2*/)->bool
    {
      // e was (just) collapsed.
      if(EQ(e) == -1) { return false; }
      Eigen::RowVectorXd p;
      double cost;
      cost_and_placement(e,V,F,E,EMAP,EF,EI,cost,p);
      return std::isinf(cost);
    };
}

IGL_INLINE igl::decimate_stopping_condition_callback
  igl::infinite_cost_stopping_condition(
  const decimate_cost_and_placement_callback & cost_and_placement)
{
  decimate_stopping_condition_callback stopping_condition;
  infinite_cost_stopping_condition(cost_and_placement,stopping_condition);
  return stopping_condition;
}

