// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "orth.h"

// Broken Implementation
IGL_INLINE void igl::orth(const Eigen::MatrixXd &A, Eigen::MatrixXd &Q)
{

  //perform svd on A = U*S*V' (V is not computed and only the thin U is computed)
  Eigen::JacobiSVD<Eigen::MatrixXd> svd(A, Eigen::ComputeThinU );
  Eigen::MatrixXd U = svd.matrixU();
  const Eigen::VectorXd S = svd.singularValues();
  
  //get rank of A
  int m = A.rows();
  int n = A.cols();
  double tol = std::max(m,n) * S.maxCoeff() *  2.2204e-16;
  int r = 0;
  for (int i = 0; i < S.rows(); ++r,++i)
  {
    if (S[i] < tol)
      break;
  }
  
  //keep r first columns of U
  Q = U.block(0,0,U.rows(),r);
}
