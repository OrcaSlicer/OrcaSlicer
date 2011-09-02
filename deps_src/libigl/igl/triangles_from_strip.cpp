// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "triangles_from_strip.h"
#include <iostream>

template <typename DerivedS, typename DerivedF>
IGL_INLINE void igl::triangles_from_strip(
  const Eigen::MatrixBase<DerivedS>& S,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  using namespace std;
  F.resize(S.size()-2,3);
  for(int s = 0;s < S.size()-2;s++)
  {
    if(s%2 == 0)
    {
      F(s,0) = S(s+2);
      F(s,1) = S(s+1);
      F(s,2) = S(s+0);
    }else
    {
      F(s,0) = S(s+0);
      F(s,1) = S(s+1);
      F(s,2) = S(s+2);
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
