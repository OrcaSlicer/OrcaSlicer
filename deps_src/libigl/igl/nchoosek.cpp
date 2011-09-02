// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti, Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "nchoosek.h"
#include <cmath>
#include <cassert>

IGL_INLINE double igl::nchoosek(const int n, const int k)
{
  if(k>n/2)
  {
    return nchoosek(n,n-k);
  }else if(k==1)
  {
    return n;
  }else
  {
    double c = 1;
    for(int i = 1;i<=k;i++)
    {
      c *= (((double)n-k+i)/((double)i));
    }
    return std::round(c);
  }
}

template < typename DerivedV, typename DerivedU>
IGL_INLINE void igl::nchoosek(
  const Eigen::MatrixBase<DerivedV> & V,
  const int k,
  Eigen::PlainObjectBase<DerivedU> & U)
{
  using namespace Eigen;
  if(V.size() == 0)
  {
    U.resize(0,k);
    return;
  }
  assert((V.cols() == 1 || V.rows() == 1) && "V must be a vector");
  U.resize(nchoosek(V.size(),k),k);
  int running_i  = 0;
  int running_j = 0;
  Matrix<typename DerivedU::Scalar,1,Dynamic> running(1,k);
  int N = V.size();
  const std::function<void(int,int)> doCombs =
    [&running,&N,&doCombs,&running_i,&running_j,&U,&V](int offset, int k)
  {
    if(k==0)
    {
      U.row(running_i) = running;
      running_i++;
      return;
    }
    for (int i = offset; i <= N - k; ++i)
    {
      running(running_j) = V(i);
      running_j++;
      doCombs(i+1,k-1);
      running_j--;
    }
  };
  doCombs(0,k);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::nchoosek<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#if EIGEN_VERSION_AT_LEAST(3,3,0)
#else
template void igl::nchoosek<Eigen::CwiseNullaryOp<Eigen::internal::linspaced_op<int, true>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::CwiseNullaryOp<Eigen::internal::linspaced_op<int, true>, Eigen::Matrix<int, -1, 1, 0, -1, 1> > > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif

#endif
