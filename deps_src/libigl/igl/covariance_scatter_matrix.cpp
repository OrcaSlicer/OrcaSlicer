// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "covariance_scatter_matrix.h"
#include "arap_linear_block.h"
#include "cotmatrix.h"
#include "sum.h"
#include "edges.h"
#include "verbose.h"
#include "cat.h"
#include "PI.h"

template <
  typename DerivedV, 
  typename DerivedF,
  typename CSM_type>
IGL_INLINE void igl::covariance_scatter_matrix(
  const Eigen::MatrixBase<DerivedV> & V, 
  const Eigen::MatrixBase<DerivedF> & F,
  const ARAPEnergyType energy,
  Eigen::SparseMatrix<CSM_type>& CSM)
{
  using namespace Eigen;
  // number of mesh vertices
  int n = V.rows();
  assert(n > F.maxCoeff());
  // dimension of mesh
  int dim = V.cols();
  // Number of mesh elements
  int m = F.rows();

  // number of rotations
  int nr;
  switch(energy)
  {
    case ARAP_ENERGY_TYPE_SPOKES:
      nr = n;
      break;
    case ARAP_ENERGY_TYPE_SPOKES_AND_RIMS:
      nr = n;
      break;
    case ARAP_ENERGY_TYPE_ELEMENTS:
      nr = m;
      break;
    default:
      fprintf(
        stderr,
        "covariance_scatter_matrix.h: Error: Unsupported arap energy %d\n",
        energy);
      return;
  }

  SparseMatrix<double> KX,KY,KZ;
  arap_linear_block(V,F,0,energy,KX);
  arap_linear_block(V,F,1,energy,KY);
  SparseMatrix<double> Z(n,nr);
  if(dim == 2)
  {
    CSM = cat(1,cat(2,KX,Z),cat(2,Z,KY)).transpose();
  }else if(dim == 3)
  {
    arap_linear_block(V,F,2,energy,KZ);
    SparseMatrix<double>ZZ(n,nr*2);
    CSM = 
      cat(1,cat(1,cat(2,KX,ZZ),cat(2,cat(2,Z,KY),Z)),cat(2,ZZ,KZ)).transpose();
  }else
  {
    fprintf(
     stderr,
     "covariance_scatter_matrix.h: Error: Unsupported dimension %d\n",
     dim);
    return;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::covariance_scatter_matrix<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, igl::ARAPEnergyType, Eigen::SparseMatrix<double, 0, int>&);
#endif
