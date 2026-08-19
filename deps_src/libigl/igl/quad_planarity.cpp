// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "quad_planarity.h"
#include <Eigen/Geometry>

template <typename DerivedV, typename DerivedF, typename DerivedP>
IGL_INLINE void igl::quad_planarity(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedP> & P)
{
  int nf = F.rows();
  P.setZero(nf,1);
  for (int i =0; i<nf; ++i)
  {
    const Eigen::Matrix<typename DerivedV::Scalar,1,3> &v1 = V.row(F(i,0));
    const Eigen::Matrix<typename DerivedV::Scalar,1,3> &v2 = V.row(F(i,1));
    const Eigen::Matrix<typename DerivedV::Scalar,1,3> &v3 = V.row(F(i,2));
    const Eigen::Matrix<typename DerivedV::Scalar,1,3> &v4 = V.row(F(i,3));
    Eigen::Matrix<typename DerivedV::Scalar,1,3> diagCross=(v3-v1).cross(v4-v2);
    typename DerivedV::Scalar denom = 
      diagCross.norm()*(((v3-v1).norm()+(v4-v2).norm())/2);
    if (fabs(denom)<1e-8)
      //degenerate quad is still planar
      P(i) = 0;
    else
      P(i) = (diagCross.dot(v2-v1)/denom);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::quad_planarity<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
