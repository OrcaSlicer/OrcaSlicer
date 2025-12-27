// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "lscm.h"

#include "vector_area_matrix.h"
#include "cotmatrix.h"
#include "repdiag.h"
#include "min_quad_with_fixed.h"
#include <iostream>

IGL_INLINE bool igl::lscm(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F,
  const Eigen::VectorXi& b,
  const Eigen::MatrixXd& bc,
  Eigen::MatrixXd & V_uv)
{
  using namespace Eigen;
  using namespace std;
  
  // Assemble the area matrix (note that A is #Vx2 by #Vx2)
  SparseMatrix<double> A;
  igl::vector_area_matrix(F,A);

  // Assemble the cotan laplacian matrix
  SparseMatrix<double> L;
  igl::cotmatrix(V,F,L);

  SparseMatrix<double> L_flat;
  repdiag(L,2,L_flat);

  VectorXi b_flat(b.size()*bc.cols(),1);
  VectorXd bc_flat(bc.size(),1);
  for(int c = 0;c<bc.cols();c++)
  {
    b_flat.block(c*b.size(),0,b.rows(),1) = c*V.rows() + b.array();
    bc_flat.block(c*bc.rows(),0,bc.rows(),1) = bc.col(c);
  }
  
  // Minimize the LSCM energy
  SparseMatrix<double> Q = -L_flat + 2.*A;
  const VectorXd B_flat = VectorXd::Zero(V.rows()*2);
  igl::min_quad_with_fixed_data<double> data;
  if(!igl::min_quad_with_fixed_precompute(Q,b_flat,SparseMatrix<double>(),true,data))
  {
    return false;
  }

  MatrixXd W_flat;
  if(!min_quad_with_fixed_solve(data,B_flat,bc_flat,VectorXd(),W_flat))
  {
    return false;
  }


  assert(W_flat.rows() == V.rows()*2);
  V_uv.resize(V.rows(),2);
  for (unsigned i=0;i<V_uv.cols();++i)
  {
    V_uv.col(V_uv.cols()-i-1) = W_flat.block(V_uv.rows()*i,0,V_uv.rows(),1);
  }
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
