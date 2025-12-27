// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READ_CSV_H
#define IGL_READ_CSV_H

#include "igl/igl_inline.h"
#include <Eigen/Core>
#include <string>
#include <vector>

namespace igl 
{
  // read a matrix from a csv file into a Eigen matrix
  // Templates:
  //   Scalar  type for the matrix
  // Inputs:
  //   str  path to .csv file
  // Outputs:
  //   M  eigen matrix 
  template <typename Scalar>
  IGL_INLINE bool readCSV(
    const std::string str, 
    Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic>& M);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readCSV.cpp"
#endif

#endif
