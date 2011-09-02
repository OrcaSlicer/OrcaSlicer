// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MAX_FACES_STOPPING_CONDITION_H
#define IGL_MAX_FACES_STOPPING_CONDITION_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <set>
#include <functional>
namespace igl
{
  // Stopping condition function compatible with igl::decimate. The outpute
  // function handle will return true if number of faces is less than max_m
  //
  // Inputs:
  //   m  reference to working variable initially should be set to current
  //    number of faces.
  //   orig_m  number (size) of original face list _**not**_ including any
  //     faces added to handle phony boundary faces connecting to point at
  //     infinity. For closed meshes it's safe to set this to F.rows()
  //   max_m  maximum number of faces
  // Outputs:
  //   stopping_condition
  //
  IGL_INLINE void max_faces_stopping_condition(
    int & m,
    const int orig_m,
    const int max_m,
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
    max_faces_stopping_condition(
      int & m,
      const int orign_m,
      const int max_m);
}

#ifndef IGL_STATIC_LIBRARY
#  include "max_faces_stopping_condition.cpp"
#endif
#endif

