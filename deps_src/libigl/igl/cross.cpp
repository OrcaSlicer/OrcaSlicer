// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "cross.h"

// http://www.antisphere.com/Wiki/tools:anttweakbar
IGL_INLINE void igl::cross(
    const double *a,
    const double *b,
    double *out)
{
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

template <
    typename DerivedA,
    typename DerivedB,
    typename DerivedC>
IGL_INLINE void igl::cross(
    const Eigen::MatrixBase<DerivedA> &A,
    const Eigen::MatrixBase<DerivedB> &B,
    Eigen::PlainObjectBase<DerivedC> &C)
{
  assert(A.cols() == 3 && "#cols should be 3");
  assert(B.cols() == 3 && "#cols should be 3");
  assert(A.rows() == B.rows() && "#rows in A and B should be equal");
  C.resize(A.rows(), 3);
  for (int d = 0; d < 3; d++)
  {
    C.col(d) =
        A.col((d + 1) % 3).array() * B.col((d + 2) % 3).array() -
        A.col((d + 2) % 3).array() * B.col((d + 1) % 3).array();
  }
}

#ifdef IGL_STATIC_LIBRARY
template void igl::cross<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> &);
template void igl::cross<Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3>> const &, Eigen::MatrixBase<Eigen::Matrix<double, 1, 3, 1, 1, 3>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> &);
template void igl::cross<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1>> &);
template void igl::cross<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const &, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3>> &);
#endif
