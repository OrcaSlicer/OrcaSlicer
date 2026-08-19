// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "gaussian_curvature.h"
#include "internal_angles.h"
#include "PI.h"
#include <iostream>
template <typename DerivedV, typename DerivedF, typename DerivedK>
IGL_INLINE void igl::gaussian_curvature(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedK> & K)
{
  using namespace Eigen;
  using namespace std;
  // internal corner angles
  Matrix<
    typename DerivedV::Scalar,
    DerivedF::RowsAtCompileTime,
    DerivedF::ColsAtCompileTime> A;
  internal_angles(V,F,A);
  K.resize(V.rows(),1);
  K.setConstant(V.rows(),1,2.*PI);
  assert(A.rows() == F.rows());
  assert(A.cols() == F.cols());
  assert(K.rows() == V.rows());
  assert(F.maxCoeff() < V.rows());
  assert(K.cols() == 1);
  const int Frows = F.rows();
  //K_G(x_i) = (2π - ∑θj)
  for(int f = 0;f<Frows;f++)
  {
    // throw normal at each corner
    for(int j = 0; j < 3;j++)
    {
      K(F(f,j),0) -=  A(f,j);
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::gaussian_curvature<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::gaussian_curvature<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
