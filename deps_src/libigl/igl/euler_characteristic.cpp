// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich <michaelrabinovich27@gmail.com@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "euler_characteristic.h"

#include "edge_topology.h"
#include "edges.h"
template <typename DerivedF>
IGL_INLINE int igl::euler_characteristic(
  const Eigen::MatrixBase<DerivedF> & F)
{
  const int nf = F.rows();
  const int nv = F.maxCoeff()+1;
  Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,2> E;
  edges(F,E);
  const int ne = E.rows();
  return nv - ne + nf;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template int igl::euler_characteristic<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
#endif
