// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "dihedral_angles.h"
#include "dihedral_angles_intrinsic.h"
#include "edge_lengths.h"
#include "face_areas.h"

#include <cassert>

template <
  typename DerivedV, 
  typename DerivedT, 
  typename Derivedtheta,
  typename Derivedcos_theta>
IGL_INLINE void igl::dihedral_angles(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedT>& T,
  Eigen::PlainObjectBase<Derivedtheta>& theta,
  Eigen::PlainObjectBase<Derivedcos_theta>& cos_theta)
{
  using namespace Eigen;
  assert(T.cols() == 4);
  Matrix<typename Derivedtheta::Scalar,Dynamic,6> l;
  edge_lengths(V,T,l);
  Matrix<typename Derivedtheta::Scalar,Dynamic,4> s;
  face_areas(l,s);
  return dihedral_angles_intrinsic(l,s,theta,cos_theta);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::dihedral_angles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
