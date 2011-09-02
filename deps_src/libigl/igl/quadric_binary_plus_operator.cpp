// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "quadric_binary_plus_operator.h"

IGL_INLINE std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double> 
  igl::operator+(
    const std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double>  & a, 
    const std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double>  & b)
{
  std::tuple<
    Eigen::MatrixXd,
    Eigen::RowVectorXd,
    double>  c;
  std::get<0>(c) = (std::get<0>(a) + std::get<0>(b)).eval();
  std::get<1>(c) = (std::get<1>(a) + std::get<1>(b)).eval();
  std::get<2>(c) = (std::get<2>(a) + std::get<2>(b));
  return c;
}

