// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SPEYE_H
#define IGL_SPEYE_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Sparse>

namespace igl
{
  // Builds an m by n sparse identity matrix like matlab's speye function
  // Templates:
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Inputs:
  //   m  number of rows
  //   n  number of cols
  // Outputs:
  //   I  m by n sparse matrix with 1's on the main diagonal
  template <typename T>
  IGL_INLINE void speye(const int n,const int m, Eigen::SparseMatrix<T> & I);
  // Builds an n by n sparse identity matrix like matlab's speye function
  // Templates:
  //   T  should be a eigen sparse matrix primitive type like int or double
  // Inputs:
  //   n  number of rows and cols
  // Outputs:
  //   I  n by n sparse matrix with 1's on the main diagonal
  template <typename T>
  IGL_INLINE void speye(const int n, Eigen::SparseMatrix<T> & I);
}

#ifndef IGL_STATIC_LIBRARY
#  include "speye.cpp"
#endif

#endif
