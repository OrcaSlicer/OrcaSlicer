// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "circumradius.h"
#include "edge_lengths.h"
#include "doublearea.h"
template <
  typename DerivedV, 
  typename DerivedF,
  typename DerivedR>
IGL_INLINE void igl::circumradius(
  const Eigen::PlainObjectBase<DerivedV> & V, 
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedR> & R)
{
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,3> l;
  igl::edge_lengths(V,F,l);
  DerivedR A;
  igl::doublearea(l,0.,A);
  // use formula: R=abc/(4*area) to compute the circum radius
  R = l.col(0).array() * l.col(1).array() * l.col(2).array() / (2.0*A.array());
}
