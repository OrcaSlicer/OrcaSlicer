// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "invert_diag.h"

template <typename T>
IGL_INLINE void igl::invert_diag(
  const Eigen::SparseMatrix<T>& X, 
  Eigen::SparseMatrix<T>& Y)
{
#ifndef NDEBUG
  typename Eigen::SparseVector<T> dX = X.diagonal().sparseView();
  // Check that there are no zeros along the diagonal
  assert(dX.nonZeros() == dX.size());
#endif
  // http://www.alecjacobson.com/weblog/?p=2552
  if(&Y != &X)
  {
    Y = X;
  }
  // Iterate over outside
  for(int k=0; k<Y.outerSize(); ++k)
  {
    // Iterate over inside
    for(typename Eigen::SparseMatrix<T>::InnerIterator it (Y,k); it; ++it)
    {
      if(it.col() == it.row())
      {
        T v = it.value();
        assert(v != 0);
        v = ((T)1.0)/v;
        Y.coeffRef(it.row(),it.col()) = v;
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::invert_diag<double>(Eigen::SparseMatrix<double, 0, int> const&, Eigen::SparseMatrix<double, 0, int>&);
template void igl::invert_diag<float>(Eigen::SparseMatrix<float, 0, int> const&, Eigen::SparseMatrix<float, 0, int>&);
#endif
