// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Stefan Brugger <stefanbrugger@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "procrustes.h"
#include "polar_svd.h"
#include "polar_dec.h"

template <
  typename DerivedX,
  typename DerivedY,
  typename Scalar,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Scalar& scale,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t)
{
  using namespace Eigen;
  assert (X.rows() == Y.rows() && "Same number of points");
  assert(X.cols() == Y.cols() && "Points have same dimensions");

  // Center data
  const VectorXd Xmean = X.colwise().mean();
  const VectorXd Ymean = Y.colwise().mean();
  MatrixXd XC = X.rowwise() - Xmean.transpose();
  MatrixXd YC = Y.rowwise() - Ymean.transpose();

  // Scale
  scale = 1.;
  if (includeScaling)
  {
     double scaleX = XC.norm() / XC.rows();
     double scaleY = YC.norm() / YC.rows();
     scale = scaleY/scaleX;
     XC *= scale;
     assert (std::abs(XC.norm() / XC.rows() - scaleY) < 1e-8);
  }

  // Rotation
  MatrixXd S = XC.transpose() * YC;
  MatrixXd T;
  if (includeReflections)
  {
    polar_dec(S,R,T);
  }else
  {
    polar_svd(S,R,T);
  }
//  R.transposeInPlace();

  // Translation
  t = Ymean - scale*R.transpose()*Xmean;
}


template <
  typename DerivedX,
  typename DerivedY,
  typename Scalar,
  int DIM,
  int TType>
IGL_INLINE void igl::procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Eigen::Transform<Scalar,DIM,TType>& T)
{
  using namespace Eigen;
  double scale;
  MatrixXd R;
  VectorXd t;
  procrustes(X,Y,includeScaling,includeReflections,scale,R,t);

  // Combine
  T = Translation<Scalar,DIM>(t) * R * Scaling(scale);
}

template <
  typename DerivedX,
  typename DerivedY,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    bool includeScaling,
    bool includeReflections,
    Eigen::PlainObjectBase<DerivedR>& S,
    Eigen::PlainObjectBase<DerivedT>& t)
{
  double scale;
  procrustes(X,Y,includeScaling,includeReflections,scale,S,t);
  S *= scale;
}

template <
  typename DerivedX,
  typename DerivedY,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t)
{
  procrustes(X,Y,false,false,R,t);
}

template <
  typename DerivedX,
  typename DerivedY,
  typename Scalar,
  typename DerivedT>
IGL_INLINE void igl::procrustes(
    const Eigen::PlainObjectBase<DerivedX>& X,
    const Eigen::PlainObjectBase<DerivedY>& Y,
    Eigen::Rotation2D<Scalar>& R,
    Eigen::PlainObjectBase<DerivedT>& t)
{
  using namespace Eigen;
  assert (X.cols() == 2 && Y.cols() == 2 && "Points must have dimension 2");
  Matrix2d Rmat;
  procrustes(X,Y,false,false,Rmat,t);
  R.fromRotationMatrix(Rmat);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::procrustes<Eigen::Matrix<double, 3, 2, 0, 3, 2>, Eigen::Matrix<double, 3, 2, 0, 3, 2>, double, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 1, 0, 2, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 2, 0, 3, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 2, 0, 3, 2> > const&, bool, bool, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 1, 0, 2, 1> >&);
#endif
