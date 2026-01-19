// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "columnize.h"
#include <cassert>

template <typename DerivedA, typename DerivedB>
IGL_INLINE void igl::columnize(
  const Eigen::PlainObjectBase<DerivedA> & A,
  const int k,
  const int dim,
  Eigen::PlainObjectBase<DerivedB> & B)
{
  // Eigen matrices must be 2d so dim must be only 1 or 2
  assert(dim == 1 || dim == 2);

  // block height, width, and number of blocks
  int m,n;
  if(dim == 1)
  {
    m = A.rows()/k;
    assert(m*(int)k == (int)A.rows());
    n = A.cols();
  }else// dim == 2
  {
    m = A.rows();
    n = A.cols()/k;
    assert(n*(int)k == (int)A.cols());
  }

  // resize output
  B.resize(A.rows()*A.cols(),1);

  for(int b = 0;b<(int)k;b++)
  {
    for(int i = 0;i<m;i++)
    {
      for(int j = 0;j<n;j++)
      {
        if(dim == 1)
        {
          B(j*m*k+i*k+b) = A(i+b*m,j);
        }else
        {
          B(j*m*k+i*k+b) = A(i,b*n+j);
        }
      }
    }
  }
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::columnize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::columnize<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, int, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
template void igl::columnize<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, int, int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&);
template void igl::columnize<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, int, int, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 1, 0, -1, 1> >&);
#endif
