// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "connect_boundary_to_infinity.h"
#include "boundary_facets.h"

template <typename DerivedF, typename DerivedFO>
IGL_INLINE void igl::connect_boundary_to_infinity(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedFO> & FO)
{
  return connect_boundary_to_infinity(F,F.maxCoeff(),FO);
}
template <typename DerivedF, typename DerivedFO>
IGL_INLINE void igl::connect_boundary_to_infinity(
  const Eigen::MatrixBase<DerivedF> & F,
  const typename DerivedF::Scalar inf_index,
  Eigen::PlainObjectBase<DerivedFO> & FO)
{
  // Determine boundary edges
  Eigen::Matrix<typename DerivedFO::Scalar,Eigen::Dynamic,Eigen::Dynamic> O;
  boundary_facets(F,O);
  FO.resize(F.rows()+O.rows(),F.cols());
  FO.topLeftCorner(F.rows(),F.cols()) = F;
  FO.bottomLeftCorner(O.rows(),O.cols()) = O.rowwise().reverse();
  FO.bottomRightCorner(O.rows(),1).setConstant(inf_index);
}

template <
  typename DerivedV, 
  typename DerivedF, 
  typename DerivedVO, 
  typename DerivedFO>
IGL_INLINE void igl::connect_boundary_to_infinity(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedVO> & VO,
  Eigen::PlainObjectBase<DerivedFO> & FO)
{
  typename DerivedV::Index inf_index = V.rows();
  connect_boundary_to_infinity(F,inf_index,FO);
  VO.resize(V.rows()+1,V.cols());
  VO.topLeftCorner(V.rows(),V.cols()) = V;
  auto inf = std::numeric_limits<typename DerivedVO::Scalar>::infinity();
  VO.row(V.rows()).setConstant(inf);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::connect_boundary_to_infinity<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
