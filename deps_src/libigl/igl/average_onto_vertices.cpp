// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "average_onto_vertices.h"

template<typename DerivedV,typename DerivedF,typename DerivedS>
IGL_INLINE void igl::average_onto_vertices(const Eigen::MatrixBase<DerivedV> &V,
  const Eigen::MatrixBase<DerivedF> &F,
  const Eigen::MatrixBase<DerivedS> &S,
  Eigen::MatrixBase<DerivedS> &SV)
{
  SV = DerivedS::Zero(V.rows(),S.cols());
  Eigen::Matrix<typename DerivedF::Scalar,Eigen::Dynamic,1> COUNT(V.rows());
  COUNT.setZero();
  for (int i = 0; i <F.rows(); ++i)
  {
    for (int j = 0; j<F.cols(); ++j)
    {
      SV.row(F(i,j)) += S.row(i);
      COUNT[F(i,j)] ++;
    }
  }
  for (int i = 0; i <V.rows(); ++i)
    SV.row(i) /= COUNT[i];
};

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
