// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mod.h"

template <typename DerivedA, typename DerivedB>
IGL_INLINE void igl::mod(
  const Eigen::PlainObjectBase<DerivedA> & A,
  const int base,
  Eigen::PlainObjectBase<DerivedB> & B)
{
  B.resizeLike(A);
  for(int i = 0;i<A.rows();i++)
  {
    for(int j = 0;j<A.cols();j++)
    {
      B(i,j) = A(i,j)%base;
    }
  }
}
template <typename DerivedA>
IGL_INLINE DerivedA igl::mod(
  const Eigen::PlainObjectBase<DerivedA> & A, const int base)
{
  DerivedA B;
  mod(A,base,B);
  return B;
}
#ifdef IGL_STATIC_LIBRARY
#endif
