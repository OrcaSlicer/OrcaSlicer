// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_IS_SYMMETRIC_H
#define IGL_IS_SYMMETRIC_H
#include "igl_inline.h"
#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Sparse>
namespace igl
{
  // Returns true if the given matrix is symmetric
  // Inputs:
  //   A  m by m matrix
  // Returns true if the matrix is square and symmetric
  template <typename AT>
  IGL_INLINE bool is_symmetric(const Eigen::SparseMatrix<AT>& A);
  // Inputs:
  //   epsilon threshold on L1 difference between A and A'
  template <typename AT, typename epsilonT>
  IGL_INLINE bool is_symmetric(const Eigen::SparseMatrix<AT>& A, const epsilonT epsilon);
  template <typename DerivedA>
  IGL_INLINE bool is_symmetric(
    const Eigen::PlainObjectBase<DerivedA>& A);
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_symmetric.cpp"
#endif

#endif
