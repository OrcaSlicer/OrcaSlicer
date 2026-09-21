// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "polar_svd3x3.h"
#include "svd3x3.h"
#ifdef __SSE__
#  include "svd3x3_sse.h"
#endif
#ifdef __AVX__
#  include "svd3x3_avx.h"
#endif

template<typename Mat>
IGL_INLINE void igl::polar_svd3x3(const Mat& A, Mat& R)
{
  // should be caught at compile time, but just to be 150% sure:
  assert(A.rows() == 3 && A.cols() == 3);

  Eigen::Matrix<typename Mat::Scalar, 3, 3> U, Vt;
  Eigen::Matrix<typename Mat::Scalar, 3, 1> S;
  svd3x3(A, U, S, Vt);
  R = U * Vt.transpose();
}

#ifdef __SSE__
template<typename T>
IGL_INLINE void igl::polar_svd3x3_sse(const Eigen::Matrix<T, 3*4, 3>& A, Eigen::Matrix<T, 3*4, 3> &R)
{
  // should be caught at compile time, but just to be 150% sure:
  assert(A.rows() == 3*4 && A.cols() == 3);

  Eigen::Matrix<T, 3*4, 3> U, Vt;
  Eigen::Matrix<T, 3*4, 1> S;
  svd3x3_sse(A, U, S, Vt);

  for (int k=0; k<4; k++)
  {
    R.block(3*k, 0, 3, 3) = U.block(3*k, 0, 3, 3) * Vt.block(3*k, 0, 3, 3).transpose();
  }

  //// test:
  //for (int k=0; k<4; k++)
  //{
  //  Eigen::Matrix3f Apart = A.block(3*k, 0, 3, 3);
  //  Eigen::Matrix3f Rpart;
  //  polar_svd3x3(Apart, Rpart);

  //  Eigen::Matrix3f Rpart_SSE = R.block(3*k, 0, 3, 3);
  //  Eigen::Matrix3f diff = Rpart - Rpart_SSE;
  //  float diffNorm = diff.norm();

  //  int hu = 1;
  //}
  //// eof test
}
#endif

#ifdef __AVX__
template<typename T>
IGL_INLINE void igl::polar_svd3x3_avx(const Eigen::Matrix<T, 3*8, 3>& A, Eigen::Matrix<T, 3*8, 3> &R)
{
  // should be caught at compile time, but just to be 150% sure:
  assert(A.rows() == 3*8 && A.cols() == 3);

  Eigen::Matrix<T, 3*8, 3> U, Vt;
  Eigen::Matrix<T, 3*8, 1> S;
  svd3x3_avx(A, U, S, Vt);

  for (int k=0; k<8; k++)
  {
    R.block(3*k, 0, 3, 3) = U.block(3*k, 0, 3, 3) * Vt.block(3*k, 0, 3, 3).transpose();
  }

}
#endif

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::polar_svd3x3<Eigen::Matrix<double, 3, 3, 0, 3, 3> >(Eigen::Matrix<double, 3, 3, 0, 3, 3> const&, Eigen::Matrix<double, 3, 3, 0, 3, 3>&);
template void igl::polar_svd3x3<Eigen::Matrix<float,3,3,0,3,3> >(Eigen::Matrix<float,3,3,0,3,3> const &,Eigen::Matrix<float,3,3,0,3,3> &);

#ifdef __SSE__
template void igl::polar_svd3x3_sse<float>(Eigen::Matrix<float, 12, 3, 0, 12, 3> const&, Eigen::Matrix<float, 12, 3, 0, 12, 3>&);
#endif

#ifdef __AVX__
template void igl::polar_svd3x3_avx<float>(Eigen::Matrix<float, 24, 3, 0, 24, 3> const&, Eigen::Matrix<float, 24, 3, 0, 24, 3>&);
#endif

#endif
