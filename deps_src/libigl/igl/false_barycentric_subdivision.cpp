// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "false_barycentric_subdivision.h"

#include "PlainMatrix.h"
#include "barycenter.h"
#include <algorithm>

template <typename DerivedV, typename DerivedF, typename DerivedVD, typename DerivedFD>
IGL_INLINE void igl::false_barycentric_subdivision(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    Eigen::PlainObjectBase<DerivedVD> & VD,
    Eigen::PlainObjectBase<DerivedFD> & FD)
{
  using namespace Eigen;
  // Compute face barycenter
  PlainMatrix<DerivedV> BC;
  igl::barycenter(V,F,BC);

  // Add the barycenters to the vertices
  VD.resize(V.rows()+F.rows(),3);
  VD.block(0,0,V.rows(),3) = V;
  VD.block(V.rows(),0,F.rows(),3) = BC;

  // Each face is split four ways
  FD.resize(F.rows()*3,3);

  for (unsigned i=0; i<F.rows(); ++i)
  {
    int i0 = F(i,0);
    int i1 = F(i,1);
    int i2 = F(i,2);
    int i3 = V.rows() + i;

    Eigen::Matrix<typename DerivedFD::Scalar,1,3> F0,F1,F2;
    F0 << i0,i1,i3;
    F1 << i1,i2,i3;
    F2 << i2,i0,i3;

    FD.row(i*3 + 0) = F0;
    FD.row(i*3 + 1) = F1;
    FD.row(i*3 + 2) = F2;
  }


}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::false_barycentric_subdivision<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
#endif
