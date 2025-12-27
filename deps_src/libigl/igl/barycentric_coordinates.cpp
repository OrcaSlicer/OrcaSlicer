// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "barycentric_coordinates.h"
#include "volume.h"

template <
  typename DerivedP,
  typename DerivedA,
  typename DerivedB,
  typename DerivedC,
  typename DerivedD,
  typename DerivedL>
IGL_INLINE void igl::barycentric_coordinates(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedA> & A,
  const Eigen::MatrixBase<DerivedB> & B,
  const Eigen::MatrixBase<DerivedC> & C,
  const Eigen::MatrixBase<DerivedD> & D,
  Eigen::PlainObjectBase<DerivedL> & L)
{
  using namespace Eigen;
  assert(P.cols() == 3 && "query must be in 3d");
  assert(A.cols() == 3 && "corners must be in 3d");
  assert(B.cols() == 3 && "corners must be in 3d");
  assert(C.cols() == 3 && "corners must be in 3d");
  assert(D.cols() == 3 && "corners must be in 3d");
  assert(P.rows() == A.rows() && "Must have same number of queries as corners");
  assert(A.rows() == B.rows() && "Corners must be same size");
  assert(A.rows() == C.rows() && "Corners must be same size");
  assert(A.rows() == D.rows() && "Corners must be same size");
  typedef Matrix<typename DerivedL::Scalar,DerivedL::RowsAtCompileTime,1> 
    VectorXS;
  // Total volume
  VectorXS vol,LA,LB,LC,LD;
  volume(B,D,C,P,LA);
  volume(A,C,D,P,LB);
  volume(A,D,B,P,LC);
  volume(A,B,C,P,LD);
  volume(A,B,C,D,vol);
  L.resize(P.rows(),4);
  L<<LA,LB,LC,LD;
  L.array().colwise() /= vol.array();
}

template <
  typename DerivedP,
  typename DerivedA,
  typename DerivedB,
  typename DerivedC,
  typename DerivedL>
IGL_INLINE void igl::barycentric_coordinates(
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedA> & A,
  const Eigen::MatrixBase<DerivedB> & B,
  const Eigen::MatrixBase<DerivedC> & C,
  Eigen::PlainObjectBase<DerivedL> & L)
{
  using namespace Eigen;
#ifndef NDEBUG
  const int DIM = P.cols();
  assert(A.cols() == DIM && "corners must be in same dimension as query");
  assert(B.cols() == DIM && "corners must be in same dimension as query");
  assert(C.cols() == DIM && "corners must be in same dimension as query");
  assert(P.rows() == A.rows() && "Must have same number of queries as corners");
  assert(A.rows() == B.rows() && "Corners must be same size");
  assert(A.rows() == C.rows() && "Corners must be same size");
#endif

  // http://gamedev.stackexchange.com/a/23745
  typedef 
    Eigen::Array<
      typename DerivedP::Scalar,
               DerivedP::RowsAtCompileTime,
               DerivedP::ColsAtCompileTime>
    ArrayS;
  typedef 
    Eigen::Array<
      typename DerivedP::Scalar,
               DerivedP::RowsAtCompileTime,
               1>
    VectorS;

  const ArrayS v0 = B.array() - A.array();
  const ArrayS v1 = C.array() - A.array();
  const ArrayS v2 = P.array() - A.array();
  VectorS d00 = (v0*v0).rowwise().sum();
  VectorS d01 = (v0*v1).rowwise().sum();
  VectorS d11 = (v1*v1).rowwise().sum();
  VectorS d20 = (v2*v0).rowwise().sum();
  VectorS d21 = (v2*v1).rowwise().sum();
  VectorS denom = d00 * d11 - d01 * d01;
  L.resize(P.rows(),3);
  L.col(1) = (d11 * d20 - d01 * d21) / denom;
  L.col(2) = (d00 * d21 - d01 * d20) / denom;
  L.col(0) = 1.0f -(L.col(1) + L.col(2)).array();
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::barycentric_coordinates<Eigen::Matrix<float, 1, -1, 1, 1, -1>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3>, Eigen::Matrix<float, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, 1, 2, 1, 1, 2>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::MatrixBase<Eigen::Block<Eigen::Matrix<double, -1, -1, 0, -1, -1> const, 1, -1, false> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
template void igl::barycentric_coordinates<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
