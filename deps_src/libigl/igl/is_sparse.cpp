// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_sparse.h"
template <typename T>
IGL_INLINE bool igl::is_sparse(const Eigen::SparseMatrix<T> &)
{
  return true;
}
template <typename DerivedA>
IGL_INLINE bool igl::is_sparse(const Eigen::MatrixBase<DerivedA>& )
{
  return false;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::is_sparse<double>(Eigen::SparseMatrix<double, 0, int> const&);
template bool igl::is_sparse<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&);
#endif
