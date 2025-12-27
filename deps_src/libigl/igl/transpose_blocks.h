// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_TRANSPOSE_BLOCKS_H
#define IGL_TRANSPOSE_BLOCKS_H
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl
{
  // Templates:
  //   T  should be a eigen matrix primitive type like int or double
  // Inputs:
  //   A  m*k by n (dim: 1) or m by n*k (dim: 2) eigen Matrix of type T values
  //   k  number of blocks
  //   dim  dimension in which to transpose
  // Output
  //   B  n*k by m (dim: 1) or n by m*k (dim: 2) eigen Matrix of type T values,
  //   NOT allowed to be the same as A
  //
  // Example:
  // A = [
  //   1   2   3   4
  //   5   6   7   8
  // 101 102 103 104
  // 105 106 107 108
  // 201 202 203 204
  // 205 206 207 208];
  // transpose_blocks(A,1,3,B);
  // B -> [
  //   1   5
  //   2   6
  //   3   7
  //   4   8
  // 101 105
  // 102 106
  // 103 107
  // 104 108
  // 201 205
  // 202 206
  // 203 207
  // 204 208];
  //   
  template <typename T>
  IGL_INLINE void transpose_blocks(
    const Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & A,
    const size_t k,
    const size_t dim,
    Eigen::Matrix<T,Eigen::Dynamic,Eigen::Dynamic> & B);
}

#ifndef IGL_STATIC_LIBRARY
#  include "transpose_blocks.cpp"
#endif

#endif
