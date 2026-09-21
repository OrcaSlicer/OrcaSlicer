// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "polar_svd.h"
#include <Eigen/SVD>
#include <Eigen/Geometry>
#include <iostream>

// Adapted from Olga's CGAL mentee's ARAP code
template <
  typename DerivedA,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::polar_svd(
  const Eigen::MatrixBase<DerivedA> & A,
  const bool includeReflections,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<DerivedT> & T)
{
  typedef 
    Eigen::Matrix<typename DerivedA::Scalar,
    DerivedA::RowsAtCompileTime,
    DerivedA::ColsAtCompileTime>
    MatA;
  MatA U;
  MatA V;
  Eigen::Matrix<typename DerivedA::Scalar,DerivedA::RowsAtCompileTime,1> S;
  return igl::polar_svd(A,includeReflections,R,T,U,S,V);
}

template <
  typename DerivedA,
  typename DerivedR,
  typename DerivedT,
  typename DerivedU,
  typename DerivedS,
  typename DerivedV>
IGL_INLINE void igl::polar_svd(
  const Eigen::MatrixBase<DerivedA> & A,
  const bool includeReflections,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<DerivedT> & T,
  Eigen::PlainObjectBase<DerivedU> & U,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedV> & V)
{
  using namespace std;
  typedef 
    Eigen::Matrix<typename DerivedA::Scalar,
    DerivedA::RowsAtCompileTime,
    DerivedA::ColsAtCompileTime>
    MatA;
  Eigen::JacobiSVD<MatA> svd;
  svd.compute(A, Eigen::ComputeFullU | Eigen::ComputeFullV );
  U = svd.matrixU();
  V = svd.matrixV();
  S = svd.singularValues();
  R = U*V.transpose();
  const auto & SVT = S.asDiagonal() * V.adjoint();
  // Check for reflection
  if(!includeReflections && R.determinant() < 0)
  {
    // Annoyingly the .eval() is necessary
    auto W = V.eval();
    W.col(V.cols()-1) *= -1.;
    R = U*W.transpose();
    T = W*SVT;
  }else
  {
    T = V*SVT;
  }
}

template <
  typename DerivedA,
  typename DerivedR,
  typename DerivedT,
  typename DerivedU,
  typename DerivedS,
  typename DerivedV>
IGL_INLINE void igl::polar_svd(
  const Eigen::MatrixBase<DerivedA> & A,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<DerivedT> & T,
  Eigen::PlainObjectBase<DerivedU> & U,
  Eigen::PlainObjectBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedV> & V)
{
  return igl::polar_svd(A,false,R,T,U,S,V);
}
/// \overload
template <
  typename DerivedA,
  typename DerivedR,
  typename DerivedT>
IGL_INLINE void igl::polar_svd(
  const Eigen::MatrixBase<DerivedA> & A,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<DerivedT> & T)
{
  return igl::polar_svd(A,false,R,T);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::polar_svd<Eigen::Transpose<Eigen::Matrix<double, 3, 3, 0, 3, 3> >, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Transpose<Eigen::Matrix<double, 3, 3, 0, 3, 3> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::polar_svd<Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&);
template void igl::polar_svd<Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::polar_svd<Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 2, 0, 2, 2> >(Eigen::MatrixBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::polar_svd<Eigen::Matrix<float,3,3,0,3,3>,Eigen::Matrix<float,3,3,0,3,3>,Eigen::Matrix<float,3,3,0,3,3>,Eigen::Matrix<float,3,3,0,3,3>,Eigen::Matrix<float,3,1,0,3,1>,Eigen::Matrix<float,3,3,0,3,3> >(Eigen::MatrixBase<Eigen::Matrix<float,3,3,0,3,3> > const &,Eigen::PlainObjectBase<Eigen::Matrix<float,3,3,0,3,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<float,3,3,0,3,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<float,3,3,0,3,3> > &,Eigen::PlainObjectBase<Eigen::Matrix<float,3,1,0,3,1> > &,Eigen::PlainObjectBase<Eigen::Matrix<float,3,3,0,3,3> >&);
template void igl::polar_svd<Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 3, 1, 0, 3, 1>, Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&);
template void igl::polar_svd<Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 2, 0, 2, 2>, Eigen::Matrix<float, 2, 1, 0, 2, 1>, Eigen::Matrix<float, 2, 2, 0, 2, 2> >(Eigen::MatrixBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 1, 0, 2, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<float, 2, 2, 0, 2, 2> >&);
template void igl::polar_svd<Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 2, 0, 2, 2>, Eigen::Matrix<double, 2, 1, 0, 2, 1>, Eigen::Matrix<double, 2, 2, 0, 2, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 1, 0, 2, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 2, 2, 0, 2, 2> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::polar_svd<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
