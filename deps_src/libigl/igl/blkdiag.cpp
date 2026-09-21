// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "blkdiag.h"

template <typename Scalar>
IGL_INLINE void igl::blkdiag(
  const std::vector<Eigen::SparseMatrix<Scalar>> & L, 
  Eigen::SparseMatrix<Scalar> & Y)
{
  int nr = 0;
  int nc = 0;
  for(const auto & A : L)
  {
    nr += A.rows();
    nc += A.cols();
  }
  Y.resize(nr,nc);
  {
    int i = 0;
    int j = 0;
    for(const auto & A : L)
    {
      for(int k = 0;k<A.outerSize();++k)
      {
        for(typename Eigen::SparseMatrix<Scalar>::InnerIterator it(A,k);it;++it)
        {
           Y.insert(i+it.row(),j+k) = it.value();
        }
      }
      i += A.rows();
      j += A.cols();
    }
  }
}

template <typename DerivedY>
IGL_INLINE void igl::blkdiag(
  const std::vector<DerivedY> & L, 
  Eigen::PlainObjectBase<DerivedY> & Y)
{
  int nr = 0;
  int nc = 0;
  for(const auto & A : L)
  {
    nr += A.rows();
    nc += A.cols();
  }
  Y.setZero(nr,nc);
  {
    int i = 0;
    int j = 0;
    for(const auto & A : L)
    {
      Y.block(i,j,A.rows(),A.cols()) = A;
      i += A.rows();
      j += A.cols();
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// explicit template instantiations
template void igl::blkdiag<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::vector<Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::allocator<Eigen::Matrix<double, -1, -1, 0, -1, -1> > > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::blkdiag<double>(std::vector<Eigen::SparseMatrix<double, 0, int>, std::allocator<Eigen::SparseMatrix<double, 0, int> > > const&, Eigen::SparseMatrix<double, 0, int>&);
#endif
