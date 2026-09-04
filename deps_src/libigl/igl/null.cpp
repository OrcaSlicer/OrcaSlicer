// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "null.h"
#include "EPS.h"

template <typename DerivedA, typename DerivedN>
IGL_INLINE void igl::null(
  const Eigen::MatrixBase<DerivedA> & A,
  Eigen::PlainObjectBase<DerivedN> & N)
{
  using namespace Eigen;
  typedef typename DerivedA::Scalar Scalar;
  JacobiSVD<Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> > svd(A, ComputeFullV);
  svd.setThreshold(A.cols() * svd.singularValues().maxCoeff() * EPS<Scalar>());
  N = svd.matrixV().rightCols(A.cols()-svd.rank());
}

#ifdef IGL_STATIC_LIBRARY
template void igl::null<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::null<Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 3, 2, 0, 3, 2> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 3, 2, 0, 3, 2> >&);
#endif
