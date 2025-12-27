// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "randperm.h"
#include "colon.h"
#include <algorithm> 

template <typename DerivedI>
IGL_INLINE void igl::randperm(
  const int n,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  Eigen::VectorXi II;
  igl::colon(0,1,n-1,II);
  I = II;
  std::random_shuffle(I.data(),I.data()+n);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::randperm<Eigen::Matrix<int, -1, 1, 0, -1, 1> >(int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::randperm<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
