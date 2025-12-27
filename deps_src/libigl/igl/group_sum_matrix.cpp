// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "group_sum_matrix.h"

template <typename T>
IGL_INLINE void igl::group_sum_matrix(
  const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
  const int k,
  Eigen::SparseMatrix<T>& A)
{
  // number of vertices
  int n = G.rows();
  assert(k > G.maxCoeff());

  A.resize(k,n);

  // builds A such that A(i,j) = 1 where i corresponds to group i and j
  // corresponds to vertex j

  // Loop over vertices
  for(int j = 0;j<n;j++)
  {
    A.insert(G(j),j) = 1;
  }

  A.makeCompressed();
}

template <typename T>
IGL_INLINE void igl::group_sum_matrix(
  const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
  Eigen::SparseMatrix<T>& A)
{
  return group_sum_matrix(G,G.maxCoeff()+1,A);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::group_sum_matrix<double>(Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, int, Eigen::SparseMatrix<double, 0, int>&);
template void igl::group_sum_matrix<double>(Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
