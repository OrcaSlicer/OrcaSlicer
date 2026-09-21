// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_ORTH_H
#define IGL_ORTH_H

#include "igl_inline.h"
#include <Eigen/Dense>

namespace igl
{
  /// Orthogonalization of a matrix. ORTH(A,Q) produces Q as an orthonormal
  /// basis for the range of A. That is, Q'*Q = I, the columns of Q span the
  /// same space as the columns of A, and the number of columns of Q is the rank
  /// of A.
  ///  
  ///  
  /// The algorithm  uses singular value decomposition, SVD, instead of
  /// orthogonal factorization, QR.  This doubles the computation time, but
  /// provides more reliable and consistent rank determination. Closely follows
  /// MATLAB implementation in orth.m
  ///
  /// @param[in] A  m by n matrix 
  /// @param[out] Q  m by n matrix with orthonormal columns spanning same column
  ///   space as A
  ///  
  /// \warning Implementation listed as "Broken"
  IGL_INLINE void orth(const Eigen::MatrixXd &A, Eigen::MatrixXd &Q);
}


#ifndef IGL_STATIC_LIBRARY
#  include "orth.cpp"
#endif
#endif

