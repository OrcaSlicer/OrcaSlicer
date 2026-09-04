// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Michael Rabinovich <michaelrabinovich27@gmail.com@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "flipped_triangles.h"

#include "list_to_matrix.h"
#include <vector>
template <typename DerivedV, typename DerivedF, typename DerivedX>
IGL_INLINE void igl::flipped_triangles(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedX> & X)
{
  assert(V.cols() == 2 && "V should contain 2D positions");
  std::vector<typename DerivedX::Scalar> flip_idx;
  for (int i = 0; i < F.rows(); i++)
  {
    // https://www.cs.cmu.edu/~quake/robust.html
    typedef Eigen::Matrix<typename DerivedV::Scalar,1,2> RowVector2S;
    RowVector2S v1_n = V.row(F(i,0));
    RowVector2S v2_n = V.row(F(i,1));
    RowVector2S v3_n = V.row(F(i,2));
    Eigen::Matrix<typename DerivedV::Scalar,3,3> T2_Homo;
    T2_Homo.col(0) << v1_n(0),v1_n(1),1.;
    T2_Homo.col(1) << v2_n(0),v2_n(1),1.;
    T2_Homo.col(2) << v3_n(0),v3_n(1),1.;
    double det = T2_Homo.determinant();
    assert(det == det && "det should not be NaN");
    if (det < 0)
    {
      flip_idx.push_back(i);
    }
  }
  igl::list_to_matrix(flip_idx,X);
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE Eigen::VectorXi igl::flipped_triangles(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F)
{
  Eigen::VectorXi X;
  flipped_triangles(V,F,X);
  return X;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::flipped_triangles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template Eigen::Matrix<int, -1, 1, 0, -1, 1> igl::flipped_triangles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
#endif
