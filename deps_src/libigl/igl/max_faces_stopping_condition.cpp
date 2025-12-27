// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "max_faces_stopping_condition.h"

IGL_INLINE void igl::max_faces_stopping_condition(
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
    const int)> & stopping_condition)
{
  stopping_condition = 
    [orig_m,max_m,&m](
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
    const int f1,
    const int f2)->bool
    {
      // Only subtract if we're collapsing a real face
      if(f1 < orig_m) m-=1;
      if(f2 < orig_m) m-=1;
      return m<=(int)max_m;
    };
}

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
  igl::max_faces_stopping_condition(
    int & m,
    const int orig_m,
    const int max_m)
{
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
    const int)> stopping_condition;
  max_faces_stopping_condition(
      m,orig_m,max_m,stopping_condition);
  return stopping_condition;
}
