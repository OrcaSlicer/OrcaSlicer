// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_GROUP_SUM_MATRIX_H
#define IGL_GROUP_SUM_MATRIX_H
#include "igl_inline.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>

namespace igl
{
  /// Builds a matrix A such that A*V computes the sum of
  /// vertices in each group specified by G
  ///
  /// @tparam T  should be a eigen sparse matrix primitive type like int or double
  /// @param[in] G  #V list of group indices (0 to k-1) for each vertex, such that vertex i 
  ///              is assigned to group G(i)
  /// @param[in] k  #groups, good choice is max(G)+1
  /// @param[out] A  #groups by #V sparse matrix such that A*V = group_sums
  ///
  template <typename T>
  IGL_INLINE void group_sum_matrix(
    const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
    const int k,
    Eigen::SparseMatrix<T>& A);
  /// \overload
  template <typename T>
  IGL_INLINE void group_sum_matrix(
    const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
    Eigen::SparseMatrix<T>& A);
}
#ifndef IGL_STATIC_LIBRARY
#  include "group_sum_matrix.cpp"
#endif
#endif
