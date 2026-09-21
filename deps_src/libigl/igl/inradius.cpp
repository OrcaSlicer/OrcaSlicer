// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "inradius.h"
#include "edge_lengths.h"
#include "doublearea.h"

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedR>
IGL_INLINE void igl::inradius(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedR> & r)
{
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,3> l;
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,1> R;
  igl::edge_lengths(V,F,l);
  // If R is the circumradius,
  // R*r = (abc)/(2*(a+b+c))
  // R = abc/(4*area)
  // r(abc/(4*area)) = (abc)/(2*(a+b+c))
  // r/(4*area) = 1/(2*(a+b+c))
  // r = (2*area)/(a+b+c)
  DerivedR A;
  igl::doublearea(l,0.,A);
  r = A.array() /l.array().rowwise().sum();
}
