// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "progressive_hulls.h"
#include "progressive_hulls_cost_and_placement.h"
#include "../decimate.h"
#include "../decimate_trivial_callbacks.h"
#include "../max_faces_stopping_condition.h"
IGL_INLINE bool igl::copyleft::progressive_hulls(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const size_t max_m,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J)
{
  int m = F.rows();
  Eigen::VectorXi I;
  decimate_pre_collapse_callback always_try;
  decimate_post_collapse_callback never_care;
  decimate_trivial_callbacks(always_try,never_care);
  return decimate(
    V,
    F,
    progressive_hulls_cost_and_placement,
    max_faces_stopping_condition(m,(const int)m,max_m),
    always_try,
    never_care,
    U,
    G,
    J,
    I);
}
