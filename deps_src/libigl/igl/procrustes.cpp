// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Stefan Brugger <stefanbrugger@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "procrustes.h"
#include "polar_dec.h"

template <
  typename DerivedX,
  typename DerivedY,
  typename Scalar,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::procrustes(
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
    Scalar& scale,
    Eigen::PlainObjectBase<DerivedR>& R,
    Eigen::PlainObjectBase<DerivedT>& t)
{
  using namespace Eigen;
  assert (X.rows() == Y.rows() && "Same number of points");
  assert(X.cols() == Y.cols() && "Points have same dimensions");

  // Center data
  const Matrix<typename DerivedX::Scalar, Dynamic, 1> Xmean = X.colwise().mean();
  const Matrix<typename DerivedY::Scalar, Dynamic, 1> Ymean = Y.colwise().mean();
  Matrix<typename DerivedX::Scalar, Dynamic, Dynamic> XC
      = X.rowwise() - Xmean.transpose();
  Matrix<typename DerivedY::Scalar, Dynamic, Dynamic> YC
      = Y.rowwise() - Ymean.transpose();

  // Rotation
  Matrix<typename DerivedX::Scalar, Dynamic, Dynamic> S = XC.transpose() * YC;
  Matrix<typename DerivedT::Scalar, Dynamic, Dynamic> T;
  polar_dec(S, includeReflections, R, T);

  // Scale
  scale = 1.;
  if (includeScaling)
  {
      scale = (R.transpose() * S).trace() / (XC.array() * XC.array()).sum();
  }

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
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
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
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
    const bool includeScaling,
    const bool includeReflections,
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
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
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
    const Eigen::MatrixBase<DerivedX>& X,
    const Eigen::MatrixBase<DerivedY>& Y,
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
template void igl::procrustes<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, double, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 1, 0, 3, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, const bool, const bool, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&);
template void igl::procrustes<Eigen::Matrix<double, 3, 2, 0, 3, 2>, Eigen::Matrix<double, 3, 2, 0, 3, 2>, double, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 1, 0, 2, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, 3, 2, 0, 3, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 3, 2, 0, 3, 2> > const&, const bool, const bool, double&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 1, 0, 2, 1> >&);
#endif
