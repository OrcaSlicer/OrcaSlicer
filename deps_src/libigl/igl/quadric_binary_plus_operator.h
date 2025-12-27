// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QUADRIC_BINARY_PLUS_OPERATOR_H
#define IGL_QUADRIC_BINARY_PLUS_OPERATOR_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <tuple>

namespace igl
{
  // A binary addition operator for Quadric tuples compatible with qslim,
  // computing c = a+b
  //
  // Inputs:
  //   a  QSlim quadric
  //   b  QSlim quadric
  // Output
  //   c  QSlim quadric
  //
  IGL_INLINE std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double> 
    operator+(
      const std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double>  & a, 
      const std::tuple< Eigen::MatrixXd, Eigen::RowVectorXd, double>  & b);
}

#ifndef IGL_STATIC_LIBRARY
#  include "quadric_binary_plus_operator.cpp"
#endif

#endif
