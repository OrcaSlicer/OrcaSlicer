// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CAT_H
#define IGL_CAT_H
#include "igl_inline.h"

#define EIGEN_YES_I_KNOW_SPARSE_MODULE_IS_NOT_STABLE_YET
#include <Eigen/Sparse>
#include <Eigen/Dense>

namespace igl
{
  // If you're using Dense matrices you might be better off using the << operator

  // This is an attempt to act like matlab's cat function.

  // Perform concatenation of a two matrices along a single dimension
  // If dim == 1, then C = [A;B]. If dim == 2 then C = [A B]
  // 
  // Template:
  //   Scalar  scalar data type for sparse matrices like double or int
  //   Mat  matrix type for all matrices (e.g. MatrixXd, SparseMatrix)
  //   MatC  matrix type for output matrix (e.g. MatrixXd) needs to support
  //     resize
  // Inputs:
  //   A  first input matrix
  //   B  second input matrix
  //   dim  dimension along which to concatenate, 1 or 2
  // Outputs:
  //   C  output matrix
  //   
  template <typename Scalar>
  IGL_INLINE void cat(
      const int dim, 
      const Eigen::SparseMatrix<Scalar> & A, 
      const Eigen::SparseMatrix<Scalar> & B, 
      Eigen::SparseMatrix<Scalar> & C);
  template <typename Derived, class MatC>
  IGL_INLINE void cat(
    const int dim,
    const Eigen::MatrixBase<Derived> & A, 
    const Eigen::MatrixBase<Derived> & B,
    MatC & C);
  // Wrapper that returns C
  template <class Mat>
  IGL_INLINE Mat cat(const int dim, const Mat & A, const Mat & B);

  // Note: Maybe we can autogenerate a bunch of overloads D = cat(int,A,B,C),
  // E = cat(int,A,B,C,D), etc. 

  // Concatenate a "matrix" of blocks
  // C = [A0;A1;A2;...;An] where Ai = [A[i][0] A[i][1] ... A[i][m]];
  //
  // Inputs:
  //   A  a matrix (vector of row vectors)
  // Output:
  //   C
  template <class Mat>
  IGL_INLINE void cat(const std::vector<std::vector< Mat > > & A, Mat & C);
}

#ifndef IGL_STATIC_LIBRARY
#  include "cat.cpp"
#endif

#endif
